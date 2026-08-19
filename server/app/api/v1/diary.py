"""
Diary endpoints.
"""

from fastapi import APIRouter

router = APIRouter()


@router.get("/list")
async def list_diary_entries(month: str = None):
    """List diary entries for a given month (YYYY-MM)."""
    return {"status": "ok", "entries": [], "message": "Not yet implemented"}


@router.get("/{entry_id}")
async def get_diary_entry(entry_id: str):
    """Get a specific diary entry."""
    return {"status": "ok", "message": "Not yet implemented"}


