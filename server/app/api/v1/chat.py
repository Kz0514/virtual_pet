"""
Chat REST endpoints — text chat with tools + conversation history.
"""

from fastapi import APIRouter, Depends, HTTPException
from pydantic import BaseModel, Field
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession
from app.api.deps import get_current_device
from app.core.database import get_db
from app.models import Device, Conversation
from app.services.llm_service import chat_with_tools
from app.utils.reply_parser import clean_text
from datetime import datetime, timezone

router = APIRouter()


class ChatSendRequest(BaseModel):
    text: str = Field(..., min_length=1, max_length=2000)
    history: list[dict] | None = None
    context: dict | None = None  # {"temp": 35, "hum": 60, ...}


class ChatReply(BaseModel):
    reply: str
    tools_used: list[str] = []


@router.post("/send", response_model=ChatReply)
async def send_message(
    req: ChatSendRequest,
    device: Device = Depends(get_current_device),
    db: AsyncSession = Depends(get_db),
):
    """Send a text message and get LLM reply with tool support."""
    # Cache sensor context if provided
    if req.context:
        from app.services.tool_service import cache_sensor
        cache_sensor(str(device.id), req.context)

    # 历史只允许 user/assistant — 防客户端注入 role:system
    history = [
        {"role": h["role"], "content": h["content"]}
        for h in (req.history or [])
        if isinstance(h, dict) and h.get("role") in ("user", "assistant")
        and isinstance(h.get("content"), str)
    ]

    try:
        reply, tools_used = await chat_with_tools(
            user_text=req.text,
            history=history,
            device_id=str(device.id),
        )
    except Exception as e:
        raise HTTPException(status_code=502, detail=f"LLM error: {e}")

    # Save conversation (assistant 内容剥协议标记)
    now = datetime.now(timezone.utc)
    db.add(Conversation(device_id=device.id, role="user", content=req.text, created_at=now))
    db.add(Conversation(device_id=device.id, role="assistant",
                        content=clean_text(reply), created_at=now))
    await db.commit()

    return ChatReply(reply=reply, tools_used=list(tools_used))


@router.get("/history")
async def get_history(
    before: str = None,
    limit: int = 20,
    device: Device = Depends(get_current_device),
    db: AsyncSession = Depends(get_db),
):
    """Get conversation history."""
    q = (
        select(Conversation)
        .where(Conversation.device_id == device.id)
        .order_by(Conversation.created_at.desc())
        .limit(min(limit, 100))
    )
    if before:
        q = q.where(Conversation.created_at < before)
    result = await db.execute(q)
    rows = result.scalars().all()
    return {
        "history": [
            {"role": r.role, "content": r.content, "created_at": r.created_at.isoformat()}
            for r in reversed(rows)
        ]
    }


