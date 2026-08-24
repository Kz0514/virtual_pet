"""Pet state management and mood transitions."""
from sqlalchemy import select
from app.models import Pet
from app.core.database import AsyncSessionLocal


async def fetch_pet_profile(db, device_id: str) -> tuple[str, str]:
    """查宠物名/主人称谓 — 提示词动态化用 (AsyncSession 禁 lazy load, 必须显式查询).

    db 传 None 时自开会话 (llm_service 等无请求会话的调用点);
    无记录/空串回退默认 ("萝莉丝", "主人").
    """
    if not device_id:
        return "萝莉丝", "主人"

    async def _query(session):
        result = await session.execute(
            select(Pet.name, Pet.owner_name).where(Pet.device_id == device_id)
        )
        return result.first()

    row = await _query(db) if db is not None else None
    if db is None:
        async with AsyncSessionLocal() as session:
            row = await _query(session)
    if row is None:
        return "萝莉丝", "主人"
    return row[0] or "萝莉丝", row[1] or "主人"
