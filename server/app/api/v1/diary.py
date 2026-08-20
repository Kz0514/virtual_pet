"""
Diary endpoints — /api/v1/diary/list + /{entry_id} + /{entry_id}/html.
鉴权: get_current_device (JWT via query token); 数据范围限定本设备.
"""
import uuid
from datetime import date

from fastapi import APIRouter, Depends, HTTPException, Query, Response
from pydantic import BaseModel
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.api.deps import get_current_device
from app.core.database import get_db
from app.models import Device, DiaryEntry
from app.services.diary_service import load_doodle_b64, render_diary_html

router = APIRouter()


class DiaryEntryOut(BaseModel):
    id: str
    entry_date: str
    title: str | None
    content: str
    doodle_url: str | None
    interaction_count: int
    mood_summary: str | None
    created_at: str


def _to_out(e: DiaryEntry) -> DiaryEntryOut:
    return DiaryEntryOut(
        id=str(e.id),
        entry_date=e.entry_date.isoformat(),
        title=e.title,
        content=e.content,
        doodle_url=e.doodle_url,
        interaction_count=e.interaction_count or 0,
        mood_summary=e.mood_summary,
        created_at=e.created_at.isoformat() if e.created_at else "",
    )


@router.get("/list")
async def list_diary_entries(
    month: str = Query(..., pattern=r"^\d{4}-\d{2}$", description="月份 YYYY-MM"),
    device: Device = Depends(get_current_device),
    db: AsyncSession = Depends(get_db),
):
    """当月日记列表 (按日期倒序)."""
    y, m = int(month[:4]), int(month[5:7])
    start = date(y, m, 1)
    end = date(y + 1, 1, 1) if m == 12 else date(y, m + 1, 1)
    result = await db.execute(
        select(DiaryEntry)
        .where(DiaryEntry.device_id == device.id,
               DiaryEntry.entry_date >= start,
               DiaryEntry.entry_date < end)
        .order_by(DiaryEntry.entry_date.desc())
    )
    entries = result.scalars().all()
    return {"status": "ok", "entries": [_to_out(e) for e in entries]}


async def _load_entry(entry_id: str, device: Device, db: AsyncSession):
    """uuid 校验 + 本设备归属查询; 失败抛 404 (与 JSON 端点同一套逻辑)."""
    try:
        eid = uuid.UUID(entry_id)
    except ValueError:
        raise HTTPException(status_code=404, detail="Entry not found")
    result = await db.execute(
        select(DiaryEntry).where(DiaryEntry.id == eid,
                                 DiaryEntry.device_id == device.id)
    )
    entry = result.scalar_one_or_none()
    if entry is None:
        raise HTTPException(status_code=404, detail="Entry not found")
    return entry


@router.get("/{entry_id}")
async def get_diary_entry(
    entry_id: str,
    device: Device = Depends(get_current_device),
    db: AsyncSession = Depends(get_db),
):
    """单篇日记详情 (uuid 校验, 不存在或非本设备 → 404)."""
    return {"status": "ok", "entry": _to_out(await _load_entry(entry_id, device, db))}


@router.get("/{entry_id}/html")
async def get_diary_html(
    entry_id: str,
    device: Device = Depends(get_current_device),
    db: AsyncSession = Depends(get_db),
):
    """单篇日记渲染为完整 HTML 单文件 (涂鸦 base64 内嵌) — 设备同步用.

    设备端保存到 /data/diary/YYYY-MM-DD.html, USB 拷出浏览器打开即一页纸.
    """
    entry = await _load_entry(entry_id, device, db)
    doodle_b64 = load_doodle_b64(entry.doodle_url)   # 缺失 → 纯文字页
    html = render_diary_html(entry, doodle_b64)
    return Response(content=html, media_type="text/html; charset=utf-8")
