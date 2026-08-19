"""
逆地址解析 (Reverse Geocoding) — 经纬度 → 地址 + 周边 POI
GET https://apis.map.qq.com/ws/geocoder/v1?location=lat,lng&key=...

API Key 来自 api.json → config.Settings.tencent_api_key
"""

import logging
import httpx
from app.config import get_settings

settings = get_settings()
logger = logging.getLogger(__name__)

GEOCODER_URL = "https://apis.map.qq.com/ws/geocoder/v1/"
TIMEOUT = 10


async def reverse_geocode(
    lat: float,
    lng: float,
    get_poi: int = 0,
    poi_options: str = None,
) -> dict:
    """
    Convert lat/lng to human-readable address, optionally with nearby POIs.
    poi_options e.g. "address_format=short;radius=5000;policy=2"
    """
    params = {
        "key":      settings.tencent_api_key,
        "location": f"{lat},{lng}",
    }
    if get_poi:
        params["get_poi"] = 1
    if poi_options:
        params["poi_options"] = poi_options
    try:
        async with httpx.AsyncClient(timeout=TIMEOUT) as client:
            r = await client.get(GEOCODER_URL, params=params)
            r.raise_for_status()
            data = r.json()
        if data.get("status") == 0:
            result = data.get("result", {})
            ad_info = result.get("ad_info", {})
            formatted = result.get("formatted_addresses", result.get("address", ""))
            return {
                "status":            "ok",
                "address":           result.get("address", ""),
                "formatted_address": formatted,
                "ad_info":           ad_info,
                "adcode":            ad_info.get("adcode", ""),
                "pois":              result.get("pois", []) if get_poi else [],
            }
        return {"status": "error", "message": data.get("message", "unknown")}
    except Exception as e:
        logger.error(f"reverse_geocode error: {e}")
        return {"status": "error", "message": str(e)}


