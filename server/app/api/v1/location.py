"""
Tencent Map location API endpoints — 5 independent services.
地点搜索 / 逆地址解析 / 地址解析 / 智能硬件定位 / 静态图

All endpoints require JWT auth. API Key → api.json → config.Settings.tencent_api_key
"""

from fastapi import APIRouter, Depends, Query
from app.api.deps import get_current_device
from app.models import Device
from app.schemas.location import NetworkLocationRequest
import app.services.geocode_service           as geocode_svc
import app.services.reverse_geocode_service   as revgeo_svc
import app.services.place_service             as place_svc
import app.services.network_location_service  as netloc_svc
import app.services.static_map_service        as staticmap_svc

router = APIRouter()


# ── 1. 地址解析 ──
@router.get("/geocode")
async def geocode(
    address: str = Query(..., description="Textual address → coordinates"),
    region: str = Query(None, description="City name to restrict search"),
    device: Device = Depends(get_current_device),
):
    return await geocode_svc.geocode(address=address, region=region)


# ── 2. 逆地址解析 ──
@router.get("/reverse_geocode")
async def reverse_geocode(
    lat: float = Query(..., ge=-90, le=90),
    lng: float = Query(..., ge=-180, le=180),
    get_poi: int = Query(0, ge=0, le=1),
    poi_options: str = Query(None),
    device: Device = Depends(get_current_device),
):
    return await revgeo_svc.reverse_geocode(
        lat=lat, lng=lng, get_poi=get_poi, poi_options=poi_options
    )


# ── 3a. 地点搜索 (周边搜索) ──
@router.get("/place/search")
async def place_search(
    keyword: str = Query(..., description="Search keyword, e.g. '咖啡厅'"),
    location: str = Query(None, description="Center: 'lat,lng'"),
    radius: int = Query(1000, ge=1, le=50000),
    page_size: int = Query(10, ge=1, le=20),
    page_index: int = Query(1, ge=1),
    filter_category: str = Query(None, description="Category filter"),
    orderby: str = Query("_distance"),
    device: Device = Depends(get_current_device),
):
    return await place_svc.search_nearby(
        keyword=keyword, location=location, radius=radius,
        page_size=page_size, page_index=page_index,
        filter_category=filter_category, orderby=orderby,
    )


# ── 3b. 地点搜索 (关键词提示) ──
@router.get("/place/suggestion")
async def place_suggestion(
    keyword: str = Query(..., description="Autocomplete keyword"),
    region: str = Query(None),
    region_fix: int = Query(0, ge=0, le=1),
    page_size: int = Query(10, ge=1, le=20),
    page_index: int = Query(1, ge=1),
    device: Device = Depends(get_current_device),
):
    return await place_svc.suggestion(
        keyword=keyword, region=region, region_fix=region_fix,
        page_size=page_size, page_index=page_index,
    )


# ── 4. 智能硬件定位 ──
@router.post("/network")
async def network_location(
    body: NetworkLocationRequest,
    device: Device = Depends(get_current_device),
):
    from app.services.tool_service import cache_device_location
    payload = body.model_dump(exclude_none=True)
    result = await netloc_svc.locate(
        device_id=payload.pop("device_id", ""),
        gpsinfo=payload.pop("gpsinfo", None),
        cellinfo=payload.pop("cellinfo", None),
        wifiinfo=payload.pop("wifiinfo", None),
        beaconinfo=payload.pop("beaconinfo", None),
        get_poi=payload.pop("get_poi", 0),
    )
    # Cache precise location from network positioning
    if result.get("status") == "ok" and result.get("lat") and result.get("lng"):
        result["source"] = "network"
        cache_device_location(str(device.id), result)
    return result


# ── 5b. 设备时区 (IP 定位缓存 → Open-Meteo 换算) ──
@router.get("/timezone")
async def device_timezone(device: Device = Depends(get_current_device)):
    """
    设备时区偏移 (秒)。用 _device_location 缓存的 lat/lng 查 Open-Meteo
    的 timezone 接口 (免费无 key); 无缓存/查询失败 → 兜底 +8 (东八区)。
    固件每日拉取一次; 腾讯网络定位仅覆盖中国 (恒 +8), 此换算用于
    通用场景, 失败兜底保证不阻塞。
    """
    from app.services.tool_service import get_device_location
    tz_offset = 28800
    loc = get_device_location(str(device.id))
    if loc and loc.get("lat") and loc.get("lng"):
        try:
            import httpx
            async with httpx.AsyncClient(timeout=5) as client:
                r = await client.get(
                    "https://api.open-meteo.com/v1/forecast",
                    params={
                        "latitude": loc["lat"],
                        "longitude": loc["lng"],
                        "timezone": "auto",
                        "current": "temperature_2m",
                    },
                )
                if r.status_code == 200:
                    tz = (r.json() or {}).get("timezone") or {}
                    off = tz.get("utc_offset_seconds")
                    if off is not None:
                        tz_offset = int(off)
        except Exception:
            pass
    return {"tz_offset_sec": tz_offset, "auto": True}


# ── 5. 静态图 ──
@router.get("/static_map")
async def static_map(
    center: str = Query(None, description="Center: 'lat,lng'"),
    zoom: int = Query(14, ge=4, le=18),
    size: str = Query("600*300"),
    maptype: str = Query("roadmap", description="roadmap / satellite / hybrid"),
    markers: str = Query(None, description="'color:red|lat,lng'"),
    labels: str = Query(None),
    path: str = Query(None),
    scale: int = Query(1, ge=1, le=2),
    img_format: str = Query("png", description="png / jpg / gif"),
    device: Device = Depends(get_current_device),
):
    url = staticmap_svc.build_url(
        center=center, zoom=zoom, size=size, maptype=maptype,
        markers=markers, labels=labels, path=path,
        scale=scale, img_format=img_format,
    )
    return {"status": "ok", "url": url}


