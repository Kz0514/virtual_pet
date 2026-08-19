"""
地点搜索 — 周边搜索 + 关键词提示
GET https://apis.map.qq.com/ws/place/v1/search
GET https://apis.map.qq.com/ws/place/v1/suggestion

API Key 来自 api.json → config.Settings.tencent_api_key
"""

import logging
import httpx
from app.config import get_settings

settings = get_settings()
logger = logging.getLogger(__name__)

PLACE_SEARCH  = "https://apis.map.qq.com/ws/place/v1/search"
PLACE_SUGGEST = "https://apis.map.qq.com/ws/place/v1/suggestion"
TIMEOUT = 10


async def search_nearby(
    keyword: str,
    location: str = None,      # "lat,lng"
    radius: int = 1000,
    page_size: int = 10,
    page_index: int = 1,
    filter_category: str = None,
    orderby: str = "_distance",
) -> dict:
    """Search for nearby places around a center point."""
    boundary = f"nearby({location},{radius})" if location else f"region({location},{radius})"
    params = {
        "key":        settings.tencent_api_key,
        "keyword":    keyword,
        "boundary":   boundary,
        "page_size":  page_size,
        "page_index": page_index,
        "orderby":    orderby,
    }
    if filter_category:
        params["filter"] = f"category={filter_category}"
    try:
        async with httpx.AsyncClient(timeout=TIMEOUT) as client:
            r = await client.get(PLACE_SEARCH, params=params)
            r.raise_for_status()
            data = r.json()
        if data.get("status") == 0:
            results = data.get("data", [])
            items = []
            for p in results:
                loc = p.get("location", {})
                items.append({
                    "id":       p.get("id", ""),
                    "title":    p.get("title", ""),
                    "address":  p.get("address", ""),
                    "category": p.get("category", ""),
                    "lat":      float(loc.get("lat", 0)),
                    "lng":      float(loc.get("lng", 0)),
                    "distance": p.get("_distance", 0),
                    "tel":      p.get("tel", ""),
                })
            return {"status": "ok", "count": data.get("count", 0), "places": items}
        return {"status": "error", "message": data.get("message", "unknown")}
    except Exception as e:
        logger.error(f"place_search error: {e}")
        return {"status": "error", "message": str(e)}


async def suggestion(
    keyword: str,
    region: str = None,
    region_fix: int = 0,
    page_size: int = 10,
    page_index: int = 1,
) -> dict:
    """Keyword autocomplete for place names."""
    params = {
        "key":         settings.tencent_api_key,
        "keyword":     keyword,
        "page_size":   page_size,
        "page_index":  page_index,
        "region_fix":  region_fix,
    }
    if region:
        params["region"] = region
    try:
        async with httpx.AsyncClient(timeout=TIMEOUT) as client:
            r = await client.get(PLACE_SUGGEST, params=params)
            r.raise_for_status()
            data = r.json()
        if data.get("status") == 0:
            results = data.get("data", [])
            items = []
            for p in results:
                loc = p.get("location", {})
                items.append({
                    "id":       p.get("id", ""),
                    "title":    p.get("title", ""),
                    "address":  p.get("address", ""),
                    "province": p.get("province", ""),
                    "city":     p.get("city", ""),
                    "category": p.get("category", ""),
                    "lat":      float(loc.get("lat", 0)),
                    "lng":      float(loc.get("lng", 0)),
                })
            return {"status": "ok", "count": data.get("count", 0), "places": items}
        return {"status": "error", "message": data.get("message", "unknown")}
    except Exception as e:
        logger.error(f"place_suggestion error: {e}")
        return {"status": "error", "message": str(e)}


