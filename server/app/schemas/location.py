"""
Pydantic schemas for location / Tencent Map API requests.
"""
from pydantic import BaseModel, Field
from typing import Optional


# ── 1. 地址解析 (Geocode) ──
class GeocodeRequest(BaseModel):
    address: str = Field(..., min_length=1, description="Textual address to geocode")
    region: Optional[str] = Field(None, description="City name to restrict search")


# ── 2. 逆地址解析 (Reverse Geocode) ──
class ReverseGeocodeRequest(BaseModel):
    lat: float = Field(..., ge=-90, le=90, description="Latitude (WGS84)")
    lng: float = Field(..., ge=-180, le=180, description="Longitude (WGS84)")
    get_poi: int = Field(0, ge=0, le=1, description="Return nearby POIs: 0=no, 1=yes")
    poi_options: Optional[str] = Field(None, description="POI options, e.g. radius=5000;policy=2")


# ── 3a. 地点搜索 ──
class PlaceSearchRequest(BaseModel):
    keyword: str = Field(..., min_length=1, description="Search keyword")
    location: Optional[str] = Field(None, description="Center point: 'lat,lng'")
    radius: int = Field(1000, ge=1, le=50000, description="Search radius (meters)")
    page_size: int = Field(10, ge=1, le=20)
    page_index: int = Field(1, ge=1)
    filter_category: Optional[str] = Field(None, description="Category filter, e.g. '美食'")
    orderby: str = Field("_distance", description="Sort order: _distance or _weight")


# ── 3b. 关键词提示 ──
class PlaceSuggestionRequest(BaseModel):
    keyword: str = Field(..., min_length=1)
    region: Optional[str] = Field(None, description="City name")
    region_fix: int = Field(0, ge=0, le=1, description="Strictly limit to region")
    page_size: int = Field(10, ge=1, le=20)
    page_index: int = Field(1, ge=1)


# ── 4. 智能硬件定位 ──

class GPSInfo(BaseModel):
    latitude: float
    longitude: float
    altitude: Optional[float] = None
    accuracy: Optional[float] = None
    speed: Optional[float] = None
    bearing: Optional[float] = None
    viewstar: Optional[int] = None
    usedstar: Optional[int] = None


class CellInfo(BaseModel):
    mcc: int
    mnc: int
    lac: int
    cellid: int
    rss: Optional[int] = None


class WifiInfo(BaseModel):
    mac: str
    rssi: Optional[int] = None


class BeaconInfo(BaseModel):
    mac: str
    major: Optional[int] = None
    minor: Optional[int] = None
    rssi: Optional[int] = None
    time: Optional[int] = None


class NetworkLocationRequest(BaseModel):
    device_id: str = Field(..., min_length=1, description="Unique device identifier")
    gpsinfo: Optional[GPSInfo] = None
    cellinfo: Optional[list[CellInfo]] = None
    wifiinfo: Optional[list[WifiInfo]] = None
    beaconinfo: Optional[list[BeaconInfo]] = None
    get_poi: int = Field(0, ge=0, le=1)


# ── 5. 静态图 ──
class StaticMapRequest(BaseModel):
    center: Optional[str] = Field(None, description="Center: 'lat,lng'")
    zoom: int = Field(14, ge=4, le=18)
    size: str = Field("600*300", description="Image size: 'width*height'")
    maptype: str = Field("roadmap", description="roadmap / satellite / hybrid")
    markers: Optional[str] = Field(None, description="Markers: 'color:red|lat,lng'")
    labels: Optional[str] = Field(None)
    path: Optional[str] = Field(None)
    scale: int = Field(1, ge=1, le=2, description="1=normal, 2=retina")
    img_format: str = Field("png", description="png / jpg / gif")


