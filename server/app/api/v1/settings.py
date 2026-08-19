"""
User settings sync endpoint.
"""

from fastapi import APIRouter

router = APIRouter()


@router.post("/sync")
async def sync_settings():
    """Sync user settings between device and server."""
    return {"status": "ok", "message": "Not yet implemented"}


