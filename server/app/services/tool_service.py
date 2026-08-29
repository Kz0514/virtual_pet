"""
Tool service facade (⑥-7 拆 tool_registry, ⑥-8 再拆 device_state_cache/future_bus 后).

工具域 → tool_registry.py (协议/注册表/handlers/engine)
状态缓存 → device_state_cache.py (sensor/location cache)
异步总线 → future_bus.py (scan/memory futures)

本模块为兼容门面: re-export 全部对外符号, 外部 import 位置保持不变:
  chat.py / location.py / weather.py / ws/router.py / llm_service.py
  / prompt_templates.py / tests/test_tool_parse.py
"""

from app.services.future_bus import (
    create_scan_future, resolve_scan_future,
    create_memory_future, resolve_memory_future,
)
from app.services.tool_registry import (
    TOOL_PATTERN, PARAM_PATTERN, MAX_ROUNDS, TOOLS,
    parse_tools, tool_list_text, execute_tools,
)
from app.services.device_state_cache import (
    cache_device_location, get_device_location,
    cache_sensor, get_cached_sensor,
)
