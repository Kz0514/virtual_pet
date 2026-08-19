"""
地址解析 (Geocoding) — 文字地址 → 经纬度
GET https://apis.map.qq.com/ws/geocoder/v1?address=...&key=...

API Key 来自 api.json → config.Settings.tencent_api_key
"""

import logging
import httpx
from app.config import get_settings

settings = get_settings()
logger = logging.getLogger(__name__)

GEOCODER_URL = "https://apis.map.qq.com/ws/geocoder/v1/"
TIMEOUT = 10


async def geocode(address: str, region: str = None) -> dict:
    """Convert a textual address to lat/lng coordinates."""
    params = {
        "key":     settings.tencent_api_key,
        "address": address,
    }
    if region:
        params["region"] = region
    try:
        async with httpx.AsyncClient(timeout=TIMEOUT) as client:
            r = await client.get(GEOCODER_URL, params=params)
            r.raise_for_status()
            data = r.json()
        if data.get("status") == 0:
            result = data.get("result", {})
            location = result.get("location", {})
            return {
                "status":      "ok",
                "lat":         float(location.get("lat", 0)),
                "lng":         float(location.get("lng", 0)),
                "title":       result.get("title", address),
                "address":     result.get("address", ""),
                "ad_info":     result.get("ad_info", {}),
                "reliability": result.get("reliability", 0),
            }
        return {"status": "error", "message": data.get("message", "unknown")}
    except Exception as e:
        logger.error(f"geocode error: {e}")
        return {"status": "error", "message": str(e)}


