"""
Qwen-Image 涂鸦服务 — 网格批量生成 + 缓存池 (token-plan 专属实例 + 独立 key).

背景: 每篇日记单独调一次图像生成太浪费配额 (2K 图 ~60-110s/次, 周配额有限,
      实测 6 次并发/超时调用可耗尽一周配额)。
方案: 一次调用生成 8x4 网格贴纸图 (32 格), PIL 裁剪成 32 张小图缓存;
      日记按 entry_id 哈希确定性取一张 — 32 篇日记只耗 1 次 API 调用。
prompt: 只画具体元素 (太阳/云朵/花朵/蛋糕等), 避开动物/人物 —
        实测 "画一只小猫" 触发 400 IPInfringementSuspect; 具体场景描述可过审。
失败 (429/审核/超时) 一律返回 None — 日记落库绝不阻断;
生成失败后冷却 1h, 防每篇日记都白等一次 90s。
并发: 模块级 asyncio.Lock, 多设备并发取图时只有第一个补货, 其余复用新池。
"""
import asyncio
import glob
import hashlib
import logging
import os
import random
import time

import dashscope
import httpx
from PIL import Image

from app.config import get_settings

logger = logging.getLogger("doodle")
settings = get_settings()

DOODLE_DIR = os.path.join(os.path.dirname(__file__), "..", "doodles")
BATCH_DIR = os.path.join(DOODLE_DIR, "batch")
DOODLE_PROBABILITY = 0.25   # 每篇日记附带涂鸦的概率
DOODLE_MODEL = "qwen-image-3.0-pro"
DOODLE_CALL_TIMEOUT = 120   # API 调用超时 (实测生成约 60-110s, 无超时会卡死任务)
DOODLE_DOWNLOAD_TIMEOUT = 30

GRID_COLS = 8               # 网格 8 列 x 4 行 = 32 张小图 (2K 图 2752x1536 → 344x384/格)
GRID_ROWS = 4
POOL_MIN = 8                # 池子低于此数且冷却期过 → 触发补货
RETRY_COOLDOWN_S = 3600     # 补货失败后的冷却 (429/审核/超时)
LAST_TRY_FILE = os.path.join(BATCH_DIR, ".last_try")

_pool_lock = asyncio.Lock()   # 并发补货互斥


def _grid_prompt() -> str:
    """网格贴纸图 prompt — 具体元素 + 明确网格结构, 规避 IP 审核."""
    elements = "太阳、云朵、彩虹、星星、月亮、花朵、小草、蘑菇、气球、蛋糕、糖果、冰淇淋、爱心、蝴蝶、小树、苹果"
    return (
        "生成一张 8 列 4 行的网格贴纸图, 共 32 个格子, 每个格子里画一个独立的"
        "可爱小涂鸦(蜡笔风格、粗线条、圆润)。"
        f"格子内容只从以下元素中选取: {elements}。"
        "每个格子只画一个元素, 32 个格子尽量不重复。"
        "不要画动物、不要画人物、不要画文字。"
        "背景纯白, 格子之间用清晰的白色分隔线, 边缘整齐适合裁剪。"
    )


def _pool_size() -> int:
    return len(glob.glob(os.path.join(BATCH_DIR, "grid_*.png")))


def _cooled_down() -> bool:
    """距上次补货尝试是否已过冷却期 (失败后 1h 内不重试)."""
    if not os.path.exists(LAST_TRY_FILE):
        return True
    try:
        mtime = float(open(LAST_TRY_FILE).read().strip())
    except (OSError, ValueError):
        return True
    return time.time() - mtime > RETRY_COOLDOWN_S


def _mark_try() -> None:
    os.makedirs(BATCH_DIR, exist_ok=True)
    with open(LAST_TRY_FILE, "w") as f:
        f.write(str(time.time()))


async def _call_image_api(prompt: str):
    """调 qwen-image, 返回响应对象; 调用前保存/恢复 dashscope 全局 base_url."""
    prev_url = dashscope.base_http_api_url
    dashscope.base_http_api_url = settings.doodle_base_url
    try:
        loop = asyncio.get_running_loop()
        return await asyncio.wait_for(
            loop.run_in_executor(
                None,
                lambda: dashscope.MultiModalConversation.call(
                    api_key=settings.doodle_api_key,
                    model=DOODLE_MODEL,
                    messages=[{"role": "user", "content": [{"text": prompt}]}],
                ),
            ),
            timeout=DOODLE_CALL_TIMEOUT,
        )
    finally:
        dashscope.base_http_api_url = prev_url


def _extract_image_url(response) -> str | None:
    if response is None or getattr(response, "status_code", 0) != 200:
        logger.error(f"Doodle API failed: {getattr(response, 'status_code', '?')} "
                     f"{str(response)[:200]}")
        return None
    content_list = response.output["choices"][0]["message"]["content"]
    for item in content_list:
        if isinstance(item, dict) and item.get("image"):
            img = item["image"]
            return img[0] if isinstance(img, list) else img
    logger.error(f"Doodle response no image: {content_list}")
    return None


async def _generate_and_split() -> int:
    """生成一批网格图并裁剪落盘. 成功返回本批张数, 失败返回 0."""
    _mark_try()
    response = await _call_image_api(_grid_prompt())
    img_url = _extract_image_url(response)
    if not img_url:
        return 0

    # 下载整张网格图
    async with httpx.AsyncClient(timeout=DOODLE_DOWNLOAD_TIMEOUT) as client:
        r = await client.get(img_url)
        if r.status_code != 200:
            logger.error(f"Doodle download failed: {r.status_code}")
            return 0
        img = Image.open(__import__("io").BytesIO(r.content)).convert("RGB")

    # 裁剪成格子
    os.makedirs(BATCH_DIR, exist_ok=True)
    w, h = img.size
    cw, ch = w // GRID_COLS, h // GRID_ROWS
    n = 0
    for row in range(GRID_ROWS):
        for col in range(GRID_COLS):
            tile = img.crop((col * cw, row * ch, (col + 1) * cw, (row + 1) * ch))
            tile.save(os.path.join(BATCH_DIR, f"grid_{n}.png"), "PNG")
            n += 1
    logger.info(f"Grid generated: {w}x{h} → {n} tiles")
    return n


async def ensure_pool() -> bool:
    """确保池子有货: 池 < POOL_MIN 且过冷却期 → 补货 (并发互斥)."""
    if _pool_size() >= POOL_MIN:
        return True
    async with _pool_lock:
        if _pool_size() >= POOL_MIN:   # 其他并发者已补货
            return True
        if not _cooled_down():
            logger.info("Doodle pool low but in cooldown — skip refill")
            return False
        try:
            await _generate_and_split()
        except Exception as e:
            logger.error(f"Grid refill failed: {e}")
    return _pool_size() > 0


async def maybe_generate_doodle(entry_id: str, title: str, content: str) -> str | None:
    """小概率配涂鸦: 从缓存池按 entry_id 哈希取一张 (确定性), 返回 URL.
    池空时阻塞补货 (首次部署后第一篇日记会等 ~90s, 之后零等待)."""
    if not settings.doodle_enabled:
        return None   # 总开关 — 网格生成效果待调优, 先禁用
    if not settings.doodle_api_key:
        logger.warning("doodle_api_key 未配置 — 跳过涂鸦")
        return None
    if random.random() > DOODLE_PROBABILITY:
        return None

    if not await ensure_pool():
        return None

    size = _pool_size()
    idx = int(hashlib.md5(entry_id.encode()).hexdigest(), 16) % size
    return f"/doodles/batch/grid_{idx}.png"
