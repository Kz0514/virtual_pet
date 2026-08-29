"""
Tencent Map weather API proxy — current conditions and forecast.

注意: 腾讯天气 API 只接受 location=lat,lng 参数 (city/adcode 已失效);
响应中 realtime 为数组, 天气字段嵌套在 infos 中。
"""
import httpx
import logging
from app.config import get_settings

settings = get_settings()
logger = logging.getLogger(__name__)

WEATHER_URL = "https://apis.map.qq.com/ws/weather/v1/"
GEO_URL    = "https://apis.map.qq.com/ws/geocoder/v1/"

# 默认坐标: 北京
DEFAULT_LAT = 39.9042
DEFAULT_LNG = 116.4074

TIMEOUT = 10


async def _get(endpoint: str, **params) -> dict:
    """Internal: call Tencent Maps API with key injection."""
    params.setdefault("key", settings.tencent_api_key)
    try:
        async with httpx.AsyncClient(timeout=TIMEOUT) as c:
            r = await c.get(endpoint, params=params)
            r.raise_for_status()
            return r.json()
    except Exception as e:
        logger.error(f"Tencent API error: {e}")
        return {"status": -1, "message": str(e)}


def _parse_weather(data: dict) -> dict:
    """解析响应: result.realtime[0].infos 嵌套格式"""
    result = data.get("result", {})
    realtime_list = result.get("realtime", [])
    if not realtime_list:
        return {"error": "No weather data"}
    rt = realtime_list[0]
    infos = rt.get("infos", {})
    return {
        "city":          rt.get("city", ""),
        "weather":       infos.get("weather", ""),
        "temperature":   str(infos.get("temperature", "")),
        "winddirection": infos.get("wind_direction", ""),
        "windpower":     infos.get("wind_power", ""),
        "humidity":      str(infos.get("humidity", "")),
        "reporttime":    rt.get("update_time", ""),
    }


async def get_current_weather(city: str = None, lat: float = None, lon: float = None) -> dict:
    """Get live weather. 天气 API 只接受 location=lat,lng."""
    if lat is None or lon is None:
        # 有adcode → 逆地理编码得坐标; 都没有 → 默认北京
        if city:
            coords = await _adcode_to_coords(city)
            if coords:
                lat, lon = coords
        if lat is None or lon is None:
            lat, lon = DEFAULT_LAT, DEFAULT_LNG

    data = await _get(WEATHER_URL, location=f"{lat},{lon}")
    if data.get("status") == 0:
        return _parse_weather(data)
    logger.warning(f"Weather API error: {data.get('message', '')}")
    return {"error": "No weather data"}


async def get_forecast(city: str = None, lat: float = None, lon: float = None, days: int = 3) -> list[dict]:
    """Get weather forecast. 天气 API 只接受 location=lat,lng."""
    if lat is None or lon is None:
        if city:
            coords = await _adcode_to_coords(city)
            if coords:
                lat, lon = coords
        if lat is None or lon is None:
            lat, lon = DEFAULT_LAT, DEFAULT_LNG

    data = await _get(WEATHER_URL, location=f"{lat},{lon}")
    if data.get("status") == 0:
        forecasts = data.get("result", {}).get("forecast", [])
        # 每条 forecast 含 infos 嵌套
        out = []
        for f in forecasts[:days]:
            infos = f.get("infos", f)
            out.append({
                "date":       f.get("date", ""),
                "weather":    infos.get("weather", ""),
                "temperature": str(infos.get("temperature", "")),
                "wind":       infos.get("wind_direction", ""),
            })
        return out
    return []


async def _adcode_to_coords(adcode: str) -> tuple | None:
    """adcode → (lat, lng) via reverse geocode. 用adcode作地址文本反查."""
    try:
        data = await _get(GEO_URL, address=str(adcode))
        if data.get("status") == 0:
            loc = data.get("result", {}).get("location", {})
            lat = float(loc.get("lat", 0))
            lng = float(loc.get("lng", 0))
            if lat and lng:
                return (lat, lng)
    except Exception:
        pass
    return None

