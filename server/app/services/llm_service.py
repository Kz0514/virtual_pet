"""
DeepSeek LLM integration with tool-loop and JSON mode.
"""
import json, logging
from openai import AsyncOpenAI
from app.config import get_settings
from app.services.tool_service import parse_tools, execute_tools, MAX_ROUNDS, TOOL_PATTERN
from app.utils.prompt_templates import build_system_prompt, apply_names

logger = logging.getLogger("llm")
settings = get_settings()

_client = AsyncOpenAI(
    api_key=settings.deepseek_api_key,
    base_url=settings.deepseek_base_url,
    timeout=30.0,
)

# ═══════════════ 流式句子切分 (延迟优化 1.0.25x) ═══════════════
# 模型输出按 JSON text 字段流式到达; 服务端按句子边界 (。！？…\n) 切分,
# 逐句经 WS chat_text 帧下发 — 固件逐句 TTS, 首句 ~2s 内出声,
# 其余句子边生成边播, 不再等 LLM 全文完成。
# 提取策略: 找最后一个 '"text"' 键 (JSON 重复键后者胜, 与 json.loads 一致),
# 转义感知解码其值; 完整性与转义由 _extract_text_value 状态机处理。
# 实测 DeepSeek 在带历史/上下文的场景会无视 JSON 指令直接输出纯文本
# (2026-08 设备日志两次对话均 tts_done:false) — 故 _stream_round 按首个
# 非空 chunk 的首字符自适应: '{' 开头走 JSON 提取, 否则按纯文本累计切句。
# 下发闸门: text 值中出现 "/tools." 即视为工具轮, 丢弃已缓存的部分句
# (工具调用协议要求 text 以 /tools. 开头, 无句子边界, 正常路径下
# 出现前不会有任何下发 — 该闸门是防御性兜底)。

_SENT_BOUNDARY = "。！？!?…\n"
_SUB_BOUNDARY = "，、；：,;: "


def _split_long(sent: str):
    """长句 (>60 字) 按次级标点二次切分 — 控制单句 TTS 时长.
    模型常连续输出带逗号无句号的长段 (实测 105 字), 单句合成过
    长: 设备端缓冲/可听性都差; 固件队列已无长度上限, 这里是从
    音频侧限制 (2026-08-24 队列截断实测后双保险)."""
    if len(sent) <= 60:
        yield sent
        return
    seg = ""
    for ch in sent:
        seg += ch
        if ch in _SUB_BOUNDARY and len(seg) >= 30:
            yield seg
            seg = ""
    if seg:
        yield seg


def completed_sentences(s: str):
    """Yield complete sentences (含结尾标点) from a streamed string.
    未成句的尾部不产出 — 调用方按字符偏移缓存, 跨 chunk 续接."""
    start = 0
    for i, ch in enumerate(s):
        if ch in _SENT_BOUNDARY:
            yield from _split_long(s[start:i + 1])
            start = i + 1


def _extract_text_value(buf: str) -> tuple[str | None, bool]:
    """从 (可能未完成的) 模型输出中提取最后一个 text 字段值.
    返回 (解码后文本, 是否完整); 未找到返回 (None, False).
    转义处理: \\" \\\\ \\/ \\n \\r \\t 解码, \\uXXXX 原样透传
    (不影响句子切分, 模型输出几乎不使用)."""
    key = '"text"'
    idx = buf.rfind(key)
    if idx < 0:
        return None, False
    i = idx + len(key)
    while i < len(buf) and buf[i] in " \t\r\n":
        i += 1
    if i >= len(buf) or buf[i] != ":":
        return None, False
    i += 1
    while i < len(buf) and buf[i] in " \t\r\n":
        i += 1
    if i >= len(buf) or buf[i] != '"':
        return None, False
    i += 1
    out = []
    while i < len(buf):
        c = buf[i]
        if c == "\\":
            if i + 1 >= len(buf):
                break  # 转义序列未完, 等下一 chunk
            n = buf[i + 1]
            if n in '"\\/':
                out.append(n)
            elif n == "n":
                out.append("\n")
            elif n == "r":
                out.append("\r")
            elif n == "t":
                out.append("\t")
            else:
                out.append("\\" + n)  # \uXXXX 等原样透传
            i += 2
            continue
        if c == '"':
            return "".join(out), True
        out.append(c)
        i += 1
    return "".join(out), False


async def _stream_round(messages, on_sentence, model: str) -> tuple[str, str, int]:
    """流式单轮 LLM 调用. 返回 (完整内容, 解码后的 text 字段, 已下发字符数).
    工具轮 (text 含 /tools.) 不下发任何句子, 已缓存的部分句一并丢弃."""
    stream = await _client.chat.completions.create(
        model=model,
        messages=messages,
        max_tokens=500,
        temperature=0.9,
        stream=True,
    )
    buf = ""
    content_parts = []
    raw_text = ""      # 当前提取到的 text 值 (解码后)
    emitted = 0        # 已下发字符数
    emittable = True   # 出现 /tools. 后置 False
    json_mode = False  # 模型有时无视 JSON 指令直接输出纯文本 (实测带历史时必发生)
    mode_set = False   # 首个非空 chunk 后定型, 流式中途不切换
    async for chunk in stream:
        delta = chunk.choices[0].delta.content or ""
        content_parts.append(delta)
        buf += delta
        if not mode_set and buf.strip():
            mode_set = True
            json_mode = buf.strip().startswith("{")
        if json_mode:
            t, _complete = _extract_text_value(buf)
            if t is None:
                continue  # text 键未到达, 等下一 chunk
        else:
            t = buf  # 纯文本模式: buf 本身就是对话内容, 直接累计切句
        raw_text = t
        if "/tools." in t:
            emittable = False
            continue
        if not emittable:
            continue
        if len(t) < emitted:
            emitted = 0  # text 键复写/重排 — 保守重扫 (防御性, 正常不触发)
        for sent in completed_sentences(t[emitted:]):
            await on_sentence(sent)
            emitted += len(sent)
    content = "".join(content_parts)
    # 轮末: 剩余未成句的尾部也下发 — 回复结尾常无标点, 不补会吞尾音
    if emittable and emitted < len(raw_text):
        tail = raw_text[emitted:]
        for seg in _split_long(tail):
            if seg.strip():
                await on_sentence(seg)
            emitted = len(raw_text)
    return content, raw_text, emitted


async def chat_with_tools(
    user_text: str,
    system_prompt: str | None = None,
    history: list[dict] | None = None,
    extra_context: str = "",
    device_id: str = "",
    on_sentence=None,
    pet_name: str | None = None,
    owner_name: str | None = None,
) -> tuple[str, list[str], bool]:
    """
    Chat with automatic tool resolution.
    Returns (reply_text, tools_used, tts_done).
    Model can request /tools.xxx,yyy → server executes → feeds back → model replies.

    pet_name/owner_name: 提示词与兜底文本用名; None 时按 device_id 查库
    (fetch_pet_profile), 无记录回退默认 ("萝莉丝"/"主人")。

    on_sentence (P1 流式): 提供时所有轮次走 stream=True, 完成的句子通过
    await on_sentence(sent) 逐句下发 (固件逐句 TTS); 工具轮不下发。
    tts_done = 最终回复的全部文本已通过 on_sentence 下发
    (固件据此跳过整段 TTS, 避免重复朗读)。未提供时行为与旧版一致。
    """
    if pet_name is None or owner_name is None:
        from app.services.pet_state_service import fetch_pet_profile
        pet_name, owner_name = await fetch_pet_profile(None, device_id)

    system = system_prompt or build_system_prompt(extra_context, device_id, pet_name, owner_name)
    messages: list[dict] = [{"role": "system", "content": system}]
    if history:
        messages.extend(history)
    messages.append({"role": "user", "content": user_text})

    tools_used: list[str] = []
    model = settings.deepseek_model

    def _tts_done(emitted: int, raw_len: int) -> bool:
        return on_sentence is not None and emitted > 0 and emitted >= raw_len

    for round_num in range(MAX_ROUNDS):
        if on_sentence:
            content, raw_text, emitted = await _stream_round(messages, on_sentence, model)
        else:
            response = await _client.chat.completions.create(
                model=model,
                messages=messages,
                max_tokens=500,
                temperature=0.9,
            )
            content = response.choices[0].message.content or ""
            raw_text, emitted = "", 0

        # Check for tool requests → [{name, params}, ...]
        calls = parse_tools(content)
        if not calls:
            return content, tools_used, _tts_done(emitted, len(raw_text))

        # Execute tools, feed results back
        results = await execute_tools(calls, device_id)
        tools_used.extend(c["name"] for c in calls)

        tool_data = "\n".join(f"[{k}] {v}" for k, v in results.items())
        messages.append({"role": "assistant", "content": content})
        messages.append({
            "role": "system",
            "content": f"[工具返回数据]\n{tool_data}\n\n请根据以上数据生成简短回复(50字内)。"
        })

    # 工具轮次耗尽: 强制最后一轮直接回复 — 绝不让 /tools.xxx 指令文本返回给用户
    logger.warning(f"Max tool rounds ({MAX_ROUNDS}) reached — forcing final reply")
    messages.append({
        "role": "system",
        "content": "工具调用次数已用完，请不要再调用工具，直接基于已有信息生成最终回复(50字内)。",
    })
    if on_sentence:
        content, raw_text, emitted = await _stream_round(messages, on_sentence, model)
    else:
        response = await _client.chat.completions.create(
            model=model,
            messages=messages,
            max_tokens=500,
            temperature=0.9,
        )
        content = response.choices[0].message.content or ""
        raw_text, emitted = "", 0
    content = TOOL_PATTERN.sub("", content).strip()   # 模型仍输出工具指令时剥除
    return (content or f"{pet_name}在想怎么回答你呢…"), tools_used, _tts_done(emitted, len(raw_text))


async def chat_json(
    system_prompt: str,
    user_content: str,
    max_tokens: int = 500,
    temperature: float = 0.3,
) -> dict:
    """
    JSON mode for structured tasks (mood analysis, summarization, proactive decisions).
    Returns parsed dict.
    """
    messages = [
        {"role": "system", "content": system_prompt},
        {"role": "user", "content": user_content},
    ]
    response = await _client.chat.completions.create(
        model=settings.deepseek_model,
        messages=messages,
        max_tokens=max_tokens,
        temperature=temperature,
        response_format={"type": "json_object"},
    )
    content = response.choices[0].message.content
    try:
        return json.loads(content) if isinstance(content, str) else {}
    except json.JSONDecodeError:
        logger.warning(f"JSON parse failed: {content[:100]}")
        return {"_raw": content}


async def compact_memory(content: str, pet_name: str | None = None, owner_name: str | None = None) -> str:
    """
    Compress oversized device memory via LLM (importance + recency weighting).
    Returns new file content (【摘要】/【重要信息】/【近期对话】三段), or "" on failure.
    """
    from app.utils.prompt_templates import MEMORY_SUMMARY_PROMPT
    from app.config import get_settings
    settings = get_settings()
    try:
        result = await chat_json(
            apply_names(MEMORY_SUMMARY_PROMPT, pet_name, owner_name),
            content,
            max_tokens=settings.memory_summary_max_tokens,
            temperature=0.3,
        )
    except Exception as e:
        logger.error(f"Memory compaction failed: {e}")
        return ""

    summary = result.get("summary", "") or ""
    facts = result.get("important_facts", [])
    recent = result.get("recent_dialogue", "") or ""
    facts_text = "\n".join(f"- {f}" for f in facts if f) if isinstance(facts, list) \
        else (str(facts) if facts else "")
    return "\n".join([
        "【摘要】",
        summary or "(无)",
        "【重要信息】",
        facts_text or "(无)",
        "【近期对话】",
        recent or "(无)",
    ])


# ── Backward-compatible simple chat ──

async def chat(
    user_text: str,
    system_prompt: str | None = None,
    history: list[dict] | None = None,
    max_tokens: int = 150,
    temperature: float = 0.9,
) -> str:
    """Simple chat without tools (backward compat)."""
    reply, _, _ = await chat_with_tools(user_text, system_prompt, history, "", "")
    return reply


