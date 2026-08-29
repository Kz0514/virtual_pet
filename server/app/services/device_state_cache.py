"""
Per-device state caches (⑥-8 从 tool_registry 拆出, 纯搬移).
sensor cache: ESP32 上报的传感器快照 (TTL 30s)
location cache: 设备位置 (network/ip/LLM 提供的分层覆盖)
"""
import logging, time

logger = logging.getLogger("tools")

# ── Per-device sensor cache ──
_sensor_cache: dict[str, dict] = {}
_sensor_ts: dict[str, float] = {}
SENSOR_TTL = 30

# ── Per-device location cache ──
_device_location: dict[str, dict] = {}
# {device_id: {lat, lng, adcode, city, province, source, updated_at}}


# ═══════════════ Sensor Cache (from ESP32) ═══════════════

def cache_sensor(device_id: str, data: dict):
    """Store latest sensor data from ESP32."""
    _sensor_cache[device_id] = data
    _sensor_ts[device_id] = time.time()


def get_cached_sensor(device_id: str) -> dict | None:
    """Get sensor data if available."""
    return _sensor_cache.get(device_id)


# ═══════════════ Location Cache Helpers ═══════════════

def cache_device_location(device_id: str, location: dict):
    """Cache device location. Higher precision overwrites lower."""
    if not device_id or not location:
        return
    lat = location.get("lat", 0)
    lng = location.get("lng", 0)
    if not lat and not lng:
        return
    prev = _device_location.get(device_id, {})
    prev_source = prev.get("source", "")
    new_source = location.get("source", location.get("_source", "unknown"))
    # network > ip — don't let ip overwrite network
    if prev_source == "network" and new_source == "ip":
        return
    _device_location[device_id] = {
        "lat":        lat,
        "lng":        lng,
        "adcode":     location.get("adcode", prev.get("adcode", "")),
        "city":       location.get("city", prev.get("city", "")),
        "province":   location.get("province", prev.get("province", "")),
        "source":     new_source,
        "accuracy":   location.get("accuracy", 0),
        "updated_at": time.time(),
    }
    logger.info(f"Location cached [{device_id[:8]}]: lat={lat} lng={lng} src={new_source}")


def get_device_location(device_id: str) -> dict | None:
    """Get cached device location, or None if never set."""
    return _device_location.get(device_id)


def _resolve_location(device_id: str, params: dict | None) -> tuple[float | None, float | None, str]:
    """
    Resolve lat/lng: LLM-provided > device cache > None.
    Returns (lat, lng, source_str).
    source_str: "user" | "device" | None
    """
    p = params or {}
    user_lat = p.get("lat")
    user_lng = p.get("lng")
    if user_lat and user_lng:
        try:
            return float(user_lat), float(user_lng), "user"
        except (ValueError, TypeError):
            pass
    loc = _device_location.get(device_id)
    if loc and loc.get("lat") and loc.get("lng"):
        return loc["lat"], loc["lng"], f"device({loc.get('source','unknown')})"
    return None, None, None
