"""
智能硬件定位 (Network Positioning) — WiFi / 基站 / 蓝牙 → 经纬度
POST https://apis.map.qq.com/ws/location/v1/network  (JSON body)

API Key 来自 api.json → config.Settings.tencent_api_key
"""

import logging
import httpx
from app.config import get_settings

settings = get_settings()
logger = logging.getLogger(__name__)

NETWORK_LOC_URL = "https://apis.map.qq.com/ws/location/v1/network"
TIMEOUT = 10


async def locate(
    device_id: str,
    gpsinfo: dict = None,
    cellinfo: list = None,
    wifiinfo: list = None,
    beaconinfo: list = None,
    get_poi: int = 0,
) -> dict:
    """
    Locate device via WiFi / cell-tower / Bluetooth fingerprints.
    At least one of gpsinfo / cellinfo / wifiinfo / beaconinfo is required.
    """
    body: dict = {"device_id": device_id}
    if gpsinfo:
        body["gpsinfo"] = gpsinfo
    if cellinfo:
        body["cellinfo"] = cellinfo
    if wifiinfo:
        body["wifiinfo"] = wifiinfo
    if beaconinfo:
        body["beaconinfo"] = beaconinfo
    if get_poi:
        body["get_poi"] = 1

    params = {"key": settings.tencent_api_key}
    try:
        async with httpx.AsyncClient(timeout=TIMEOUT) as client:
            r = await client.post(NETWORK_LOC_URL, params=params, json=body)
            r.raise_for_status()
            data = r.json()
        if data.get("status") == 0:
            result = data.get("result", {})
            loc = result.get("location", {})
            return {
                "status":   "ok",
                "lat":      float(loc.get("lat", 0)),
                "lng":      float(loc.get("lng", 0)),
                "accuracy": result.get("accuracy", 0),
                "ad_info":  result.get("ad_info", {}),
                "pois":     result.get("pois", []),
                "address":  result.get("address", ""),
            }
        return {"status": "error", "message": data.get("message", "unknown")}
    except Exception as e:
        logger.error(f"network_location error: {e}")
        return {"status": "error", "message": str(e)}


