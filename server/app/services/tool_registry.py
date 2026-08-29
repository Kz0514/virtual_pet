"""
Tool registry & execution engine (⑥-7 从 tool_service 拆出, 纯搬移; ⑥-8 状态
缓存迁 device_state_cache, 异步总线迁 future_bus — 以共享实例导入保持行为不变).
Protocol: model outputs /tools.name(key=val,...) → server executes → feeds back.

格式:
  /tools.weather,state           — 无参数（兼容旧格式）
  /tools.place_search(keyword=咖啡厅) — 带参数（位置自动回填）
  /tools.place_search(keyword=咖啡厅,lat=39.98,lng=116.30) — 完整参数

API Key 来自 api.json → config.Settings
"""
import re, json, time, uuid, asyncio, logging

from app.services.device_state_cache import (
    _sensor_cache, _sensor_ts, _device_location, _resolve_location,
)
from app.services.future_bus import _memory_futures, create_memory_future

logger = logging.getLogger("tools")

# ── Tool protocol ──
TOOL_PATTERN = re.compile(r"/tools\.([a-z_]+)(?:\(([^)]*)\))?")
PARAM_PATTERN = re.compile(r"(\w+)=([^,)]+)")
MAX_ROUNDS = 3

# ── Registry ──
TOOLS: dict = {}

def _register(name: str, description: str):
    """Decorator to register a tool handler. Handler: async fn(device_id, params) -> str"""
    def wrapper(fn):
        TOOLS[name] = {"name": name, "description": description, "handler": fn}
        return fn
    return wrapper


# ═══════════════ Tool Handlers ═══════════════

@_register("weather", "当前天气(天气/温度/湿度/风向)")
async def tool_weather(device_id: str, params: dict | None = None) -> str:
    from app.services.weather_service import get_current_weather
    try:
        loc = _device_location.get(device_id, {})
        lat = loc.get("lat")
        lng = loc.get("lng")
        adcode = loc.get("adcode", "")
        if lat and lng:
            w = await get_current_weather(lat=lat, lon=lng)
        elif adcode:
            w = await get_current_weather(city=adcode)
        else:
            w = await get_current_weather(city="110101")
        if w and "error" not in w:
            w["_source"] = "device" if (lat or adcode) else "default"
            return json.dumps(w, ensure_ascii=False)
    except Exception as e:
        logger.error(f"weather tool failed: {e}")
    return json.dumps({"weather": "未知", "temperature": "?", "humidity": "?"})


@_register("env", "环境数据(温度/湿度/光照/噪音/电量)")
async def tool_env(device_id: str, params: dict | None = None) -> str:
    ctx = _sensor_cache.get(device_id, {})
    if ctx:
        age = time.time() - _sensor_ts.get(device_id, 0)
        ctx["_age_sec"] = int(age)
        return json.dumps(ctx, ensure_ascii=False)
    return json.dumps({"temp": "?", "hum": "?", "light": "?", "noise": "?", "battery": "?", "_note": "暂无传感器数据"})


@_register("state", "宠物状态(心情/精力/亲密度/等级)")
async def tool_state(device_id: str, params: dict | None = None) -> str:
    from app.core.database import AsyncSessionLocal
    from sqlalchemy import text
    try:
        async with AsyncSessionLocal() as db:
            r = await db.execute(
                text("SELECT mood, energy, intimacy, level, name FROM pets WHERE device_id = :did"),
                {"did": device_id}
            )
            row = r.fetchone()
            if row:
                return json.dumps({
                    "name": row[4], "mood": row[0], "energy": row[1],
                    "intimacy": row[2], "level": row[3]
                }, ensure_ascii=False)
    except Exception as e:
        logger.error(f"state tool failed: {e}")
    return json.dumps({"mood": 50, "energy": 80, "intimacy": 50})


@_register("location", "设备所在城市(IP定位)")
async def tool_location(device_id: str, params: dict | None = None) -> str:
    loc = _device_location.get(device_id)
    if loc:
        src = loc.get("source", "unknown")
        note = ""
        if src == "ip":
            note = "IP定位精度仅到城市(误差数公里), 建议用 /tools.network_location 获取精确位置"
        elif src == "network":
            note = f"WiFi网络定位, 精度约{loc.get('accuracy',0)}米"
        return json.dumps({
            "city":     loc.get("city", "未知"),
            "province": loc.get("province", ""),
            "adcode":   loc.get("adcode", ""),
            "lat":      loc.get("lat"),
            "lng":      loc.get("lng"),
            "accuracy": loc.get("accuracy", 0),
            "_source":  src,
            "_note":    note,
        }, ensure_ascii=False)
    # Fallback: try weather API (backward compat)
    from app.services.weather_service import get_current_weather
    try:
        w = await get_current_weather()
        if w and "city" in w:
            return json.dumps({"city": w.get("city", "未知"), "_source": "weather_fallback"}, ensure_ascii=False)
    except Exception:
        pass
    return json.dumps({"city": "未知", "_hint": "设备尚未定位"}, ensure_ascii=False)


@_register("actions", "宠物动画表(名称/描述/心情/触觉)")
async def tool_actions(device_id: str, params: dict | None = None) -> str:
    """Return 11 animation slots. 播完自动回idle(zhanli站立)."""
    animations = [
        {"name":"idle","text":"站立(zhanli)","mood_fit":"平静、日常","haptic":"none",
         "use_when":"默认状态、平常说话、被动回应"},
        {"name":"happy","text":"高兴讲解","mood_fit":"开心","haptic":"short",
         "use_when":"被夸、收到好消息、用户开心时"},
        {"name":"sad","text":"难过(同站立)","mood_fit":"难过","haptic":"soft",
         "use_when":"被骂、用户生气、宠物做错事"},
        {"name":"excited","text":"抱胸说话","mood_fit":"非常开心","haptic":"heartbeat",
         "use_when":"被夸可爱、超开心、元气满满"},
        {"name":"surprised","text":"蹲着/惊讶","mood_fit":"惊讶","haptic":"double",
         "use_when":"意外消息、用户突然出现"},
        {"name":"sleepy","text":"睡觉","mood_fit":"困倦","haptic":"none",
         "use_when":"深夜、宠物累了、用户说晚安"},
        {"name":"eating","text":"吃东西","mood_fit":"美食相关","haptic":"short",
         "use_when":"聊到食物、宠物饿了"},
        {"name":"blush","text":"腼腆笑","mood_fit":"害羞、感动","haptic":"soft",
         "use_when":"被过度夸奖、用户说肉麻的话"},
        {"name":"pathead","text":"摸头","mood_fit":"亲昵","haptic":"short",
         "use_when":"用户摸头、安抚、亲昵互动"},
        {"name":"scratch","text":"挠头","mood_fit":"困惑","haptic":"short",
         "use_when":"被问倒、疑惑、不好意思"},
        {"name":"pointself","text":"指着自己","mood_fit":"俏皮","haptic":"short",
         "use_when":"用户问是谁、自夸、卖萌"},
    ]
    return json.dumps(animations, ensure_ascii=False)


@_register("history", "完整对话记忆(摘要/重要信息/近期对话, 存在设备上)")
async def tool_history(device_id: str, params: dict | None = None) -> str:
    """Async tool: WS 双向拉取设备端记忆, 超 100KB 触发 LLM 压缩并推回覆盖."""
    from app.api.ws.router import send_to_device, is_device_connected
    from app.config import get_settings
    settings = get_settings()

    if not is_device_connected(device_id):
        return json.dumps({
            "status": "error",
            "_hint": "设备不在线，暂时无法读取记忆。请先让设备连接。",
        }, ensure_ascii=False)

    req_id = uuid.uuid4().hex[:12]
    future = create_memory_future(req_id)
    await send_to_device(device_id, {"type": "get_memory", "req_id": req_id})
    logger.info(f"Sent get_memory to device {device_id[:8]} (req={req_id})")

    try:
        result = await asyncio.wait_for(future, timeout=15.0)
    except asyncio.TimeoutError:
        _memory_futures.pop(req_id, None)
        logger.warning(f"Memory read timeout for {device_id[:8]}")
        return json.dumps({
            "status": "error",
            "_hint": "读取设备记忆超时(15秒)，请稍后重试",
        }, ensure_ascii=False)

    content = result.get("content", "")
    size = result.get("size", len(content))
    if size > settings.memory_max_bytes:
        # 超限: LLM 按重要性+时间远近压缩 → 推回设备覆盖 → 用压缩后内容回答
        logger.info(f"Memory {size}B > {settings.memory_max_bytes}B — compacting")
        from app.services.llm_service import compact_memory
        from app.services.pet_state_service import fetch_pet_profile
        pet_name, owner_name = await fetch_pet_profile(None, device_id)
        new_content = await compact_memory(content, pet_name, owner_name)
        if new_content:
            await send_to_device(device_id, {"type": "memory_update", "content": new_content})
            # 重要历史落库 — 供夜间日记生成作素材 (失败不阻断对话)
            try:
                from app.core.database import AsyncSessionLocal
                from app.services.memory_service import upsert_device_memory
                async with AsyncSessionLocal() as db:
                    await upsert_device_memory(db, device_id, new_content)
                    await db.commit()
                logger.info(f"Important memory saved for {device_id[:8]}")
            except Exception as e:
                logger.warning(f"Memory save fail: {e}")
            return new_content
        logger.warning("Memory compaction failed — returning raw content")
    return content or json.dumps({"_hint": "设备记忆为空"}, ensure_ascii=False)


# ═══════════════ 腾讯地图 API 工具 ═══════════════

@_register("geocode", "地址→经纬度(地址解析)")
async def tool_geocode(device_id: str, params: dict | None = None) -> str:
    from app.services.geocode_service import geocode
    address = (params or {}).get("address", "")
    if not address:
        return json.dumps({
            "_help": "请带参数调用: /tools.geocode(address=地址名称)",
            "_example": "/tools.geocode(address=北京市朝阳区)",
        }, ensure_ascii=False)
    result = await geocode(address=address)
    return json.dumps(result, ensure_ascii=False)


@_register("reverse_geocode", "经纬度→地址(逆地址解析,lat/lng可省略)")
async def tool_reverse_geocode(device_id: str, params: dict | None = None) -> str:
    from app.services.reverse_geocode_service import reverse_geocode
    lat, lng, source = _resolve_location(device_id, params)
    if lat is None or lng is None:
        return json.dumps({
            "_help": "请带参数: /tools.reverse_geocode(lat=纬度,lng=经度) 或等待设备定位",
            "_example": "/tools.reverse_geocode(lat=39.98,lng=116.30)",
        }, ensure_ascii=False)
    result = await reverse_geocode(lat=lat, lng=lng, get_poi=1)
    result["_source"] = source
    return json.dumps(result, ensure_ascii=False)


@_register("place_search", "周边地点搜索(附近有什么,lat/lng可省略)")
async def tool_place_search(device_id: str, params: dict | None = None) -> str:
    from app.services.place_service import search_nearby
    p = params or {}
    keyword = p.get("keyword", "")
    if not keyword:
        return json.dumps({
            "_help": "请带参数: /tools.place_search(keyword=关键词,lat=纬度,lng=经度,radius=半径米)",
            "_example": "/tools.place_search(keyword=咖啡厅,radius=2000)",
            "_hint": "lat/lng可以省略，自动使用设备位置",
        }, ensure_ascii=False)
    lat, lng, source = _resolve_location(device_id, params)
    if lat is None or lng is None:
        return json.dumps({
            "_help": f"请提供位置: /tools.place_search(keyword={keyword},lat=纬度,lng=经度)",
            "_hint": "设备位置未知，请先在参数中提供lat/lng",
        }, ensure_ascii=False)
    try:
        radius = int(p.get("radius", 1000))
    except (ValueError, TypeError):
        radius = 1000
    location = f"{lat},{lng}"
    result = await search_nearby(keyword=keyword, location=location, radius=radius, page_size=5)
    result["_source"] = source
    return json.dumps(result, ensure_ascii=False)


@_register("static_map", "生成地图图片URL(lat/lng可省略)")
async def tool_static_map(device_id: str, params: dict | None = None) -> str:
    from app.services.static_map_service import build_url
    lat, lng, source = _resolve_location(device_id, params)
    if lat is None or lng is None:
        return json.dumps({
            "_help": "请带参数: /tools.static_map(lat=纬度,lng=经度,zoom=缩放)",
            "_example": "/tools.static_map(lat=39.98,lng=116.30,zoom=14)",
            "_hint": "lat/lng可以省略，自动使用设备位置",
        }, ensure_ascii=False)
    p = params or {}
    center = f"{lat},{lng}"
    zoom = int(p.get("zoom", 14)) if p.get("zoom") else 14
    url = build_url(center=center, zoom=zoom)
    return json.dumps({"status": "ok", "url": url, "_source": source}, ensure_ascii=False)


# 未启用: 腾讯智能硬件定位 API 需额外申请权限 — 原 tool_network_location
# 已删 (⑥-5); scan_wifi 请求端随之无调用方, 但 ws/router scan_result
# 接收端 + resolve_scan_future 保留为协议防御 (固件可能响应)


# ═══════════════ Engine ═══════════════

def parse_tools(text: str) -> list[dict]:
    """
    Extract tool calls from text. Returns list of {name, params}.

    支持格式:
      /tools.weather,state           → [{name:"weather",params:{}}, {name:"state",params:{}}]
      /tools.geocode(address=北京)    → [{name:"geocode",params:{address:"北京"}}]
      /tools.weather,geocode(address=北京) → 混合
    """
    results = []
    for m in TOOL_PATTERN.finditer(text):
        name = m.group(1)
        if name not in TOOLS:
            continue
        params: dict = {}
        raw_params = m.group(2)
        if raw_params:
            for pm in PARAM_PATTERN.finditer(raw_params):
                params[pm.group(1)] = pm.group(2).strip()
        results.append({"name": name, "params": params})
    return results


def tool_list_text() -> str:
    """Generate tool reference for system prompt."""
    lines = []
    for name, info in TOOLS.items():
        lines.append(f"  /tools.{name} — {info['description']}")
    return "\n".join(lines)


async def execute_tools(calls: list[dict], device_id: str) -> dict[str, str]:
    """
    Execute multiple tools in parallel. calls = [{name, params}, ...]
    Returns {name: result_json_string}.
    """
    async def run_one(call: dict):
        name = call["name"]
        params = call.get("params", {})
        try:
            result = await TOOLS[name]["handler"](device_id, params)
            return name, result
        except Exception as e:
            logger.error(f"Tool '{name}' error: {e}")
            return name, f"错误: {e}"

    tasks = [run_one(c) for c in calls]
    results_list = await asyncio.gather(*tasks)
    return dict(results_list)
