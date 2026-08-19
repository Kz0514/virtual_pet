"""
静态图 API V2 — 生成地图图片 URL（不下载图片，只返回 URL）
GET https://apis.map.qq.com/ws/staticmap/v2/?center=...&zoom=...&size=...&key=...

API Key 来自 api.json → config.Settings.tencent_api_key
"""

import logging
from urllib.parse import urlencode
from app.config import get_settings

settings = get_settings()
logger = logging.getLogger(__name__)

STATIC_MAP_URL = "https://apis.map.qq.com/ws/staticmap/v2/"


def build_url(
    center: str = None,
    zoom: int = 14,
    size: str = "600*300",
    maptype: str = "roadmap",
    markers: str = None,
    labels: str = None,
    path: str = None,
    scale: int = 1,
    img_format: str = "png",
) -> str:
    """
    Build a signed static map image URL.
    Returns fully-qualified URL ready for <img> tags or HTTP fetch.
    """
    params = {
        "key":    settings.tencent_api_key,
        "size":   size,
        "zoom":   zoom,
        "scale":  scale,
        "format": img_format,
        "maptype": maptype,
    }
    if center:
        params["center"] = center
    if markers:
        params["markers"] = markers
    if labels:
        params["labels"] = labels
    if path:
        params["path"] = path

    url = f"{STATIC_MAP_URL}?{urlencode(params)}"
    logger.info(f"static_map: {url[:120]}...")
    return url


