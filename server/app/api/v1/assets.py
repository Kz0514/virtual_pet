"""
Asset OTA endpoints — SPIFFS resource files (animation frames, etc.)
Public (no auth needed for download, list needs token)
"""
import os, hashlib, logging
from fastapi import APIRouter, Depends, HTTPException
from fastapi.responses import FileResponse
from app.api.deps import get_current_device
from app.models import Device

logger = logging.getLogger("assets_api")
router = APIRouter()

# Directory on server where asset files live (same as spiffs/ source)
ASSETS_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "..", "..", "spiffs")


@router.get("/list")
async def asset_list(device: Device = Depends(get_current_device)):
    """List available SPIFFS assets with SHA256 for diff."""
    files = []
    if not os.path.isdir(ASSETS_DIR):
        return {"files": files}

    for name in sorted(os.listdir(ASSETS_DIR)):
        path = os.path.join(ASSETS_DIR, name)
        if not os.path.isfile(path) or not name.endswith(".bin"):
            continue
        size = os.path.getsize(path)
        sha = hashlib.sha256()
        with open(path, "rb") as f:
            while True:
                chunk = f.read(65536)
                if not chunk: break
                sha.update(chunk)
        files.append({
            "name": name,
            "size": size,
            "sha256": sha.hexdigest(),
        })

    return {"files": files}


@router.get("/download/{filename}")
async def asset_download(filename: str):
    """Download a single asset file. Public — ESP32 may not have token yet."""
    path = os.path.join(ASSETS_DIR, filename)
    if not os.path.isfile(path) or not filename.endswith(".bin"):
        raise HTTPException(404, "Asset not found")
    return FileResponse(path, media_type="application/octet-stream", filename=filename)


