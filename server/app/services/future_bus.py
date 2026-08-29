"""
Async future bus (⑥-8 拆出, 纯搬移) — WS 双向请求的等待队列.

scan future   按 device_id 键控 — WS scan_wifi 请求等待固件 scan_result
memory future 按 req_id 键控 — tool_history 请求等待固件 memory_data
               (同设备并发工具请求互不覆盖)
"""
import asyncio

# ── Async scan futures (WS scan_wifi 协议防御链 — 固件可能响应 scan_result) ──
_scan_futures: dict[str, asyncio.Future] = {}

# ── Async memory futures (tool_history 双向请求) ──
_memory_futures: dict[str, asyncio.Future] = {}


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


def create_memory_future(req_id: str) -> asyncio.Future:
    """Create a Future for tool_history to await."""
    loop = asyncio.get_running_loop()
    future = loop.create_future()
    _memory_futures[req_id] = future
    return future


def resolve_memory_future(req_id: str, result: dict):
    """Called by WS handler when ESP32 sends memory_data."""
    future = _memory_futures.pop(req_id, None)
    if future and not future.done():
        future.set_result(result)
