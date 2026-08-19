"""
OTA firmware management — upload, check, download.
"""
import os, hashlib, logging
from datetime import datetime, timezone

from fastapi import APIRouter, Depends, HTTPException, UploadFile, File, Query
from fastapi.responses import FileResponse
from sqlalchemy import select, text
from sqlalchemy.ext.asyncio import AsyncSession
from pydantic import BaseModel, Field

from app.api.deps import get_current_device
from app.core.database import get_db
from app.models import Device, Firmware

logger = logging.getLogger("ota")
router = APIRouter()

FIRMWARE_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "..", "firmware")


class FirmwareInfo(BaseModel):
    version: str
    changelog: str | None = None
    file_size: int | None = None
    sha256: str


@router.get("/check")
async def check_update(
    current_version: str = Query("1.0.0"),
    db: AsyncSession = Depends(get_db),
):
    """Check if a newer firmware version is available."""
    result = await db.execute(
        select(Firmware)
        .where(Firmware.is_active == True)
        .order_by(Firmware.created_at.desc())
        .limit(1)
    )
    latest = result.scalar_one_or_none()

    if latest is None:
        return {"update_available": False, "current_version": current_version}

    # Parse version as tuple of ints for proper comparison
    try:
        cur = tuple(int(x) for x in current_version.split("."))
        lat = tuple(int(x) for x in latest.version.split("."))
        update_available = lat > cur
    except ValueError:
        update_available = latest.version != current_version

    return {
        "update_available": update_available,
        "current_version": current_version,
        "latest_version": latest.version if update_available else current_version,
        "changelog": latest.changelog if update_available else None,
        "file_size": latest.file_size if update_available else None,
        "sha256": latest.sha256_hash if update_available else None,
    }


@router.get("/download/{version}")
async def download_firmware(
    version: str,
    db: AsyncSession = Depends(get_db),
):
    """Download firmware binary."""
    result = await db.execute(
        select(Firmware).where(Firmware.version == version, Firmware.is_active == True)
    )
    fw = result.scalar_one_or_none()
    if fw is None:
        raise HTTPException(status_code=404, detail="Firmware not found")

    file_path = os.path.join(FIRMWARE_DIR, f"{fw.version}.bin")
    if not os.path.exists(file_path):
        raise HTTPException(status_code=404, detail="Firmware file missing on server")

    return FileResponse(
        file_path,
        media_type="application/octet-stream",
        filename=f"virtualpet_{fw.version}.bin",
    )


@router.post("/upload")
async def upload_firmware(
    file: UploadFile = File(...),
    version: str = Query(...),
    changelog: str = Query(""),
    db: AsyncSession = Depends(get_db),
    device: Device = Depends(get_current_device),
):
    """Upload a new firmware binary (admin use)."""
    if not file.filename or not file.filename.endswith(".bin"):
        raise HTTPException(status_code=400, detail="Only .bin files accepted")

    os.makedirs(FIRMWARE_DIR, exist_ok=True)

    # Save file
    content = await file.read()
    file_path = os.path.join(FIRMWARE_DIR, f"{version}.bin")
    with open(file_path, "wb") as f:
        f.write(content)

    # Compute SHA256
    sha256 = hashlib.sha256(content).hexdigest()
    file_size = len(content)

    # Upsert firmware record
    result = await db.execute(
        select(Firmware).where(Firmware.version == version)
    )
    existing = result.scalar_one_or_none()

    if existing:
        existing.sha256_hash = sha256
        existing.file_size = file_size
        existing.changelog = changelog or existing.changelog
        existing.file_url = f"/api/v1/ota/download/{version}"
        existing.is_active = True
        existing.created_at = datetime.now(timezone.utc)
    else:
        fw = Firmware(
            version=version,
            changelog=changelog,
            file_url=f"/api/v1/ota/download/{version}",
            file_size=file_size,
            sha256_hash=sha256,
            is_active=True,
        )
        db.add(fw)

    await db.commit()

    return {
        "status": "ok",
        "version": version,
        "file_size": file_size,
        "sha256": sha256,
    }


