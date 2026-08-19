"""
Shared FastAPI dependencies — JWT auth, device lookup, DB session.
"""

from fastapi import WebSocket, HTTPException, Query, Depends, status
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession
from app.core.database import get_db
from app.core.security import verify_access_token
from app.models import Device


async def get_current_device(
    token: str = Query(""),
    db: AsyncSession = Depends(get_db),
) -> Device:
    """REST dependency: validate JWT and return Device."""
    if not token:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Missing token",
        )
    payload = verify_access_token(token)
    if payload is None:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Invalid or expired token",
        )
    device_id = payload.get("sub")
    result = await db.execute(select(Device).where(Device.id == device_id))
    device = result.scalar_one_or_none()
    if device is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail="Device not found",
        )
    return device


async def ws_get_device(
    websocket: WebSocket,
    token: str = Query(""),
) -> Device | None:
    """WebSocket auth: validate JWT from query param. Returns None on failure."""
    if not token:
        return None
    payload = verify_access_token(token)
    if payload is None:
        return None
    device_id = payload.get("sub")
    # WS handler manages its own DB session
    from app.core.database import AsyncSessionLocal
    async with AsyncSessionLocal() as db:
        result = await db.execute(select(Device).where(Device.id == device_id))
        device = result.scalar_one_or_none()
        # Detach from session — caller manages the object
        if device:
            db.expunge(device)
        return device


