"""
LLM 回复解析与清洗 — 纯标准库, 无框架依赖 (可本地单测)。

核心保证: 任何情况下原始 JSON 结构都不得进入 chat_text (否则会被 TTS 朗读出来)。
"""
import json, logging, re

logger = logging.getLogger("reply_parser")

_CLEAN_PATTERNS = [
    re.compile(r"\|p\d+"),                        # TTS 停顿标记
    re.compile(r"\[inst:[^\]]*\]"),               # 语音指令标签
    re.compile(r"/tools\.[a-z_]+(?:\([^)]*\))?(?:,[a-z_]+)*"),  # 工具调用标记(含逗号连写)
]
_TEXT_FIELD = re.compile(r'"text"\s*:\s*"((?:[^"\\]|\\.)*)"')

FALLBACK_TEXT = "萝莉丝没听清呢，再说一次好吗~"


def clean_text(s: str) -> str:
    """Strip protocol markers from model text (for DB storage / history)."""
    for pat in _CLEAN_PATTERNS:
        s = pat.sub("", s)
    return s.strip()


def _extract_text(meta) -> str | None:
    if not isinstance(meta, dict):
        return None
    t = meta.get("text")
    return t.strip() if isinstance(t, str) and t.strip() else None


def parse_reply(reply: str) -> tuple[str, dict]:
    """
    Parse model reply into (chat_text, meta).

    兜底链 (逐级降级):
      1. 整段即 JSON
      2. 前导 JSON 对象 + 尾随内容 (raw_decode)
      3. 首行是 JSON
      4. 畸形 JSON: 正则直接提取 "text" 字段
      5. 剥掉行首 {...} 头, 取其后的人话
      6. 无 JSON 结构 → 纯文本原样使用; 有 JSON 结构但不可解析 → 兜底语
    """
    stripped = reply.strip()
    # 1) 整段即 JSON
    try:
        meta = json.loads(stripped)
        t = _extract_text(meta)
        if t is not None:
            return t, meta
    except (json.JSONDecodeError, ValueError):
        pass
    # 2) 前导 JSON 对象 + 尾随内容
    try:
        meta, _ = json.JSONDecoder().raw_decode(stripped)
        t = _extract_text(meta)
        if t is not None:
            return t, meta
    except (json.JSONDecodeError, ValueError):
        pass
    # 3) 首行是 JSON
    if "\n" in stripped:
        try:
            first_line = stripped.split("\n", 1)[0].strip()
            meta = json.loads(first_line)
            t = _extract_text(meta)
            if t is not None:
                return t, meta
        except (json.JSONDecodeError, ValueError):
            pass
    # 4) 畸形 JSON: 正则直接提取 "text" 字段
    m = _TEXT_FIELD.search(stripped)
    if m:
        try:
            return json.loads(f'"{m.group(1)}"'), {}   # 还原转义
        except (json.JSONDecodeError, ValueError):
            pass
    # 5) 剥掉行首 {...} 头, 取其后的人话
    if stripped.startswith("{"):
        end = stripped.find("}")
        if end != -1:
            rest = stripped[end + 1:].strip()
            if rest and not rest.startswith("{"):
                return rest, {}
        logger.warning(f"Reply unparseable, fallback used: {stripped[:200]}")
        return FALLBACK_TEXT, {}
    # 6) 纯文本回复 (无 JSON 结构), 原样使用
    return stripped, {}
