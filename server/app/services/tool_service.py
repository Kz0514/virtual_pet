"""
Tool service facade (⑥-7 拆 tool_registry 后).

工具域 (协议/注册表/handlers/engine/location&sensor cache/memory futures)
已迁至 tool_registry.py — 纯搬移, 函数签名照抄。
本模块保留 scan future 协议防御链 (ws scan_wifi 响应可能到来),
并 re-export 全部工具域符号, 外部 import 位置保持不变:
  chat.py / location.py / weather.py / ws/router.py / llm_service.py
  / prompt_templates.py / tests/test_tool_parse.py
"""
import asyncio, logging

logger = logging.getLogger("tools")

# ── Async scan futures (WS scan_wifi 协议防御链 — 固件可能响应 scan_result) ──
_scan_futures: dict[str, asyncio.Future] = {}


def create_scan_future(device_id: str) -> asyncio.Future:
    """Create a Future for the WS scan_wifi flow to await."""
    loop = asyncio.get_running_loop()
    future = loop.create_future()
    _scan_futures[device_id] = future
    return future


def resolve_scan_future(device_id: str, result: dict):
    """Called by WS handler when ESP32 sends scan_result."""
    future = _scan_futures.pop(device_id, None)
    if future and not future.done():
        future.set_result(result)


# ═══════════════ Re-exports from tool_registry ═══════════════

from app.services.tool_registry import (
    TOOL_PATTERN, PARAM_PATTERN, MAX_ROUNDS, TOOLS,
    cache_device_location, get_device_location,
    cache_sensor, get_cached_sensor,
    create_memory_future, resolve_memory_future,
    parse_tools, tool_list_text, execute_tools,
)
