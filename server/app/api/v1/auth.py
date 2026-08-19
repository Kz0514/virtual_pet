"""
Device registration and authentication endpoints.
"""

from fastapi import APIRouter, Depends, HTTPException, status
from pydantic import BaseModel, Field
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession
from app.core.database import get_db
from app.core.security import create_access_token, verify_access_token
from app.models import Device, Pet
from datetime import timedelta
from app.config import get_settings

router = APIRouter()
settings = get_settings()


# ── Request / Response Schemas ──

class DeviceRegisterRequest(BaseModel):
    mac_address: str = Field(..., pattern=r"^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$")
    device_name: str = Field(default="萝莉丝", max_length=64)
    firmware_version: str = Field(default="1.0.0", max_length=16)
    hardware_rev: str = Field(default="A", max_length=8)


class DeviceRegisterResponse(BaseModel):
    device_id: str
    token: str
    token_type: str = "bearer"


class DeviceTokenRequest(BaseModel):
    mac_address: str = Field(..., pattern=r"^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$")


class DeviceTokenResponse(BaseModel):
    token: str
    token_type: str = "bearer"


# ── Endpoints ──

@router.post("/register", response_model=DeviceRegisterResponse)
async def register_device(
    req: DeviceRegisterRequest,
    db: AsyncSession = Depends(get_db),
):
    """Register a new device (first-time boot) or return existing token."""
    # Check if device already exists
    result = await db.execute(
        select(Device).where(Device.mac_address == req.mac_address)
    )
    device = result.scalar_one_or_none()

    if device is None:
        # Create new device + pet
        device = Device(
            mac_address=req.mac_address,
            device_name=req.device_name,
            firmware_version=req.firmware_version,
            hardware_rev=req.hardware_rev,
        )
        db.add(device)
        await db.flush()

        pet = Pet(device_id=device.id, name=req.device_name)
        db.add(pet)
        await db.flush()

    # Issue JWT token
    token = create_access_token(
        data={"sub": str(device.id), "mac": req.mac_address},
        expires_delta=timedelta(minutes=settings.jwt_expire_minutes),
    )

    return DeviceRegisterResponse(device_id=str(device.id), token=token)


@router.post("/token", response_model=DeviceTokenResponse)
async def refresh_token(
    req: DeviceTokenRequest,
    db: AsyncSession = Depends(get_db),
):
    """Refresh a device's access token."""
    result = await db.execute(
        select(Device).where(Device.mac_address == req.mac_address)
    )
    device = result.scalar_one_or_none()
    if device is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail="Device not registered",
        )

    token = create_access_token(
        data={"sub": str(device.id), "mac": req.mac_address},
        expires_delta=timedelta(minutes=settings.jwt_expire_minutes),
    )

    return DeviceTokenResponse(token=token)


