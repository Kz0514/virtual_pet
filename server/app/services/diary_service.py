"""
日记 HTML 渲染 — 单文件信纸页 (文字 + 底部涂鸦 base64 内嵌).

设备端把渲染好的 HTML 同步到 /data/diary/YYYY-MM-DD.html,
用户经 USB 拷出后双击浏览器打开即"一页纸" (文字 + 涂鸦), 无需依赖服务器.
涂鸦加载失败/缺失 → 页面降级为纯文字, 绝不 500.
"""
import base64
import logging
import os
from html import escape
from urllib.parse import unquote

logger = logging.getLogger("diary_html")

DOODLE_DIR = os.path.join(os.path.dirname(__file__), "..", "doodles")
MAX_DOODLE_BYTES = 256 * 1024   # 涂鸦过大直接放弃内嵌 (防 HTML 膨胀)

# 信纸风样式 — 普通字符串常量, 避免 f-string 转义花括号
_CSS = """
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body { background: #f3ead8; font-family: "Kaiti SC","STKaiti","楷体",serif;
           color: #4a3f35; }
    .page { max-width: 560px; margin: 24px auto; background: #fffdf7;
            padding: 40px 36px; border-radius: 12px;
            box-shadow: 0 2px 12px rgba(0,0,0,.08);
            border-top: 6px solid #e8b4b8; }
    .date { text-align: center; color: #b0855c; font-size: 14px;
            letter-spacing: 4px; }
    h1 { text-align: center; font-size: 24px; margin: 14px 0 6px;
         font-weight: normal; }
    .mood { display: table; background: #fde8e0; color: #d96459;
            border-radius: 999px; padding: 3px 14px; font-size: 13px;
            margin: 0 auto 20px; }
    .content { line-height: 1.9; font-size: 16px; }
    .content p { margin: 10px 0; text-indent: 2em; }
    .doodle { margin-top: 28px; text-align: center; }
    .doodle img { max-width: 100%; border-radius: 8px;
                  background: #fff; box-shadow: 0 1px 6px rgba(0,0,0,.1); }
    .footer { margin-top: 36px; text-align: center; color: #c9b8a3;
              font-size: 12px; letter-spacing: 2px; }
"""


def _safe_doodle_path(doodle_url: str) -> str | None:
    """doodle_url (如 /doodles/batch/grid_3.png) → 本地绝对路径.

    路径必须落在 DOODLE_DIR 内 (防穿越), 非法返回 None.
    """
    if not doodle_url or not doodle_url.startswith("/doodles/"):
        return None
    rel = unquote(doodle_url[len("/doodles/"):])   # 先解码 %2e%2e 等, 再防穿越
    full = os.path.realpath(os.path.join(DOODLE_DIR, rel))
    root = os.path.realpath(DOODLE_DIR)
    if full != root and not full.startswith(root + os.sep):
        return None
    return full


def load_doodle_b64(doodle_url: str | None) -> str | None:
    """读取涂鸦 → data URI; 缺失/越界/过大/解码失败 → None (页面降级)."""
    if not doodle_url:
        return None
    path = _safe_doodle_path(doodle_url)
    if not path or not os.path.isfile(path):
        return None
    try:
        with open(path, "rb") as f:
            data = f.read()
        if not data or len(data) > MAX_DOODLE_BYTES:
            return None
        return "data:image/png;base64," + base64.b64encode(data).decode("ascii")
    except (OSError, ValueError):
        logger.warning("Doodle read failed: %s", doodle_url)
        return None


def render_diary_html(entry, doodle_b64: str | None) -> str:
    """渲染单篇日记为完整 HTML 文档 (所有内容内嵌, 打开即用).

    entry: 具备 entry_date/title/content/mood_summary 属性的对象
           (DiaryEntry ORM 或测试替身); 所有文本 html.escape 防 XSS.
    """
    date_str = entry.entry_date.isoformat()
    title = escape(entry.title) if entry.title else f"{date_str} 的一天"
    mood = escape(entry.mood_summary) if entry.mood_summary else ""
    paras = [escape(line) for line in entry.content.split("\n") if line.strip()]
    content_html = "".join(f"<p>{p}</p>" for p in paras) or "<p>今天没有留下文字。</p>"

    mood_html = f'<div class="mood">☀ {mood}</div>' if mood else ""
    doodle_html = (
        f'<div class="doodle"><img src="{doodle_b64}" alt="今天的涂鸦"></div>'
        if doodle_b64 else ""
    )

    return f"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title}</title>
<style>{_CSS}</style>
</head>
<body>
<div class="page">
  <div class="date">{date_str}</div>
  <h1>{title}</h1>
  {mood_html}
  <div class="content">{content_html}</div>
  {doodle_html}
  <div class="footer">Virtualpet · 萝莉丝的日记</div>
</div>
</body>
</html>
"""
