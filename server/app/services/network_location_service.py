"""
定位服务 (⑥-9 并入 IP 定位) — 腾讯定位 API 统一入口:
  locate()          智能硬件定位 — WiFi / 基站 / 蓝牙 → 经纬度
                     POST https://apis.map.qq.com/ws/location/v1/network (JSON body)
  get_ip_location() IP 定位 — 设备公网 IP → adcode + lat/lng (weather_service 迁入)

API Key 来自 api.json → config.Settings.tencent_api_key
"""

import logging
import httpx
from app.config import get_settings

settings = get_settings()
logger = logging.getLogger(__name__)

NETWORK_LOC_URL = "https://apis.map.qq.com/ws/location/v1/network"
IP_LOC_URL      = "https://apis.map.qq.com/ws/location/v1/ip"
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


# ═══════════════ IP 定位 (⑥-9 从 weather_service 并入, 签名照抄) ═══════════════

async def get_ip_location(client_ip: str = None) -> dict:
    """IP-based geolocation → adcode + lat/lng. Called by ESP32 on first boot."""
    params = {}
    if client_ip:
        params["ip"] = client_ip
    try:
        async with httpx.AsyncClient(timeout=TIMEOUT) as c:
            r = await c.get(IP_LOC_URL, params={"key": settings.tencent_api_key, **params})
            r.raise_for_status()
            data = r.json()
    except Exception as e:
        logger.error(f"Tencent API error: {e}")
        data = {"status": -1, "message": str(e)}
    if data.get("status") == 0:
        result = data.get("result", {})
        ad_info = result.get("ad_info", {})
        loc = result.get("location", {})
        adcode = ad_info.get("adcode", 110101)
        return {
            "city":     ad_info.get("city", "北京"),
            "adcode":   str(adcode),
            "province": ad_info.get("province", ""),
            "lat":      float(loc.get("lat", 0)),
            "lng":      float(loc.get("lng", 0)),
        }
    return {"city": "北京", "adcode": "110101", "province": "北京", "lat": 0, "lng": 0}
