"""
Weather proxy endpoints — Tencent Map / QQ Location Service.
Requires JWT auth (token query param).
Response format matches ESP32 cJSON parsing in main.c.
"""

from fastapi import APIRouter, Query, Request, Depends
from app.api.deps import get_current_device
from app.models import Device
from app.services.weather_service import get_current_weather, get_forecast, get_ip_location
from app.services.tool_service import cache_device_location

router = APIRouter()


@router.get("/ip_location")
async def ip_location(request: Request, device: Device = Depends(get_current_device)):
    """IP-based geolocation. ESP32 calls this first to get adcode."""
    # Use X-Forwarded-For (set by nginx) to get ESP32's real IP
    fwd = request.headers.get("X-Forwarded-For")
    real = request.headers.get("X-Real-IP")
    client_ip = (fwd or real or request.client.host) if request.client else None
    # X-Forwarded-For can be comma-separated list; take first
    if client_ip and "," in client_ip:
        client_ip = client_ip.split(",")[0].strip()
    result = await get_ip_location(client_ip)
    # Cache device location for LLM tools
    if result.get("lat") and result.get("lng"):
        result["source"] = "ip"
        cache_device_location(str(device.id), result)
    return result


@router.get("/current")
async def current_weather(
    city: str = Query(None),
    lat: float = Query(None),
    lon: float = Query(None),
    device: Device = Depends(get_current_device),
):
    """Get current weather. ESP32 parses: weather, temperature, humidity, city."""
    weather = await get_current_weather(city=city, lat=lat, lon=lon)
    if weather and "error" not in weather:
        return {
            "city":          weather.get("city", ""),
            "weather":       weather.get("weather", ""),
            "temperature":   weather.get("temperature", ""),
            "winddirection": weather.get("winddirection", ""),
            "windpower":     weather.get("windpower", ""),
            "humidity":      weather.get("humidity", ""),
            "reporttime":    weather.get("reporttime", ""),
        }
    return {"city": "北京", "weather": "晴", "temperature": "25", "humidity": "50"}


@router.get("/forecast")
async def weather_forecast(
    city: str = Query(None),
    lat: float = Query(None),
    lon: float = Query(None),
    days: int = Query(3, ge=1, le=4),
    device: Device = Depends(get_current_device),
):
    """Get weather forecast."""
    forecast = await get_forecast(city=city, lat=lat, lon=lon, days=days)
    return {"status": "ok", "forecast": forecast or []}


