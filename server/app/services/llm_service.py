"""
DeepSeek LLM integration with tool-loop and JSON mode.
"""
import json, logging
from openai import AsyncOpenAI
from app.config import get_settings
from app.services.tool_service import parse_tools, execute_tools, MAX_ROUNDS, TOOL_PATTERN
from app.utils.prompt_templates import build_system_prompt

logger = logging.getLogger("llm")
settings = get_settings()

_client = AsyncOpenAI(
    api_key=settings.deepseek_api_key,
    base_url=settings.deepseek_base_url,
    timeout=30.0,
)


async def chat_with_tools(
    user_text: str,
    system_prompt: str | None = None,
    history: list[dict] | None = None,
    extra_context: str = "",
    device_id: str = "",
) -> tuple[str, list[str]]:
    """
    Chat with automatic tool resolution.
    Returns (reply_text, tools_used).
    Model can request /tools.xxx,yyy → server executes → feeds back → model replies.
    """
    system = system_prompt or build_system_prompt(extra_context, device_id)
    messages: list[dict] = [{"role": "system", "content": system}]
    if history:
        messages.extend(history)
    messages.append({"role": "user", "content": user_text})

    tools_used: list[str] = []

    for round_num in range(MAX_ROUNDS):
        response = await _client.chat.completions.create(
            model=settings.deepseek_model,
            messages=messages,
            max_tokens=500,
            temperature=0.9,
        )
        content = response.choices[0].message.content or ""

        # Check for tool requests → [{name, params}, ...]
        calls = parse_tools(content)
        if not calls:
            return content, tools_used

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
    response = await _client.chat.completions.create(
        model=settings.deepseek_model,
        messages=messages,
        max_tokens=500,
        temperature=0.9,
    )
    content = response.choices[0].message.content or ""
    content = TOOL_PATTERN.sub("", content).strip()   # 模型仍输出工具指令时剥除
    return content or "萝莉丝在想怎么回答你呢…", tools_used


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


async def compact_memory(content: str) -> str:
    """
    Compress oversized device memory via LLM (importance + recency weighting).
    Returns new file content (【摘要】/【重要信息】/【近期对话】三段), or "" on failure.
    """
    from app.utils.prompt_templates import MEMORY_SUMMARY_PROMPT
    from app.config import get_settings
    settings = get_settings()
    try:
        result = await chat_json(
            MEMORY_SUMMARY_PROMPT,
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
    reply, _ = await chat_with_tools(user_text, system_prompt, history, "", "")
    return reply


