"""
DeviceMemory 落库 helper — 设备端记忆压缩结果 (重要历史) upsert 到服务端.

落库点 (均需自带 session, 由调用方 commit):
  1. tool_service.tool_history — 压缩成功后 (推送 memory_update 的同时)
  2. ws/router.py chat 处理 — 固件每轮带 mem_summary 时
Celery 任务 (diary_tasks) 自建 engine 场景下也复用本函数。
"""
import logging

from sqlalchemy import select
from app.models import DeviceMemory

logger = logging.getLogger("memory")


async def upsert_device_memory(db, device_id: str, content: str) -> None:
    """Upsert 压缩后的记忆内容 (device_id 主键, 存在则覆盖为最新摘要)."""
    if not content or not content.strip():
        return
    row = (
        await db.execute(select(DeviceMemory).where(DeviceMemory.device_id == device_id))
    ).scalar_one_or_none()
    if row:
        row.content = content
    else:
        db.add(DeviceMemory(device_id=device_id, content=content))
