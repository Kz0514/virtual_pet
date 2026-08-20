"""
夜间日记生成任务 — Celery worker + beat (每小时自查, 到生成时段才工作).

素材: 近 3 天对话 (聚焦 target_date 当天) + 重要历史 (device_memories) + 当天互动事件
门槛: target_date 当天 user 轮数 + 互动事件数 ≥ settings.diary_min_interactions
生成: DeepSeek JSON mode → 兜底模板 → Upsert diary_entries (device_id+entry_date 唯一)

时区: 上海 = UTC+8 (东八区无夏令时, 恒 8 小时 — 不依赖 tzdata/zoneinfo)。
Celery 任务跑在独立进程 (worker 容器), asyncio.run 新 loop —
SQLAlchemy 每次 create_async_engine + finally dispose, 不跨调用复用。
"""
import asyncio
import logging
import uuid
from datetime import date, datetime, timedelta, timezone

from sqlalchemy.ext.asyncio import create_async_engine
from sqlalchemy import text

from app.config import get_settings
from app.core.celery_app import celery_app

logger = logging.getLogger("diary_tasks")
settings = get_settings()

# 上海时区 = UTC+8 (无夏令时)
SHIFT = timedelta(hours=8)

# 计入"互动"的传感器事件 (light_change/noise 非主动互动, 不计)
INTERACTIVE_EVENTS = ("shake", "tap", "petting", "voice")

EVENT_LABELS = {
    "shake": "摇晃", "tap": "敲击", "petting": "摸头", "voice": "语音",
    "light_change": "光线变化", "noise": "噪音",
}

MAX_DIALOGUE_CHARS = 6000   # 对话素材截断上限


def diary_window_utc(target_date: date) -> tuple[datetime, datetime]:
    """上海时区 target_date 一整天 → UTC 起止时刻 (左闭右开)."""
    start = (
        datetime(target_date.year, target_date.month, target_date.day,
                 tzinfo=timezone.utc)
        - SHIFT
    )
    return start, start + timedelta(days=1)


@celery_app.task
def generate_daily_diaries():
    """Celery 任务入口 — beat 每小时触发, 到生成时段才工作."""
    asyncio.run(_run_all())


async def _run_all() -> None:
    now_sh = datetime.now(timezone.utc) + SHIFT
    if now_sh.hour != settings.diary_generate_hour:
        return
    target_date = (now_sh - timedelta(days=1)).date()
    logger.info(f"Diary generation window open — target {target_date}")

    engine = create_async_engine(settings.database_url)
    try:
        async with engine.begin() as conn:
            rows = await conn.execute(text("SELECT id FROM devices WHERE is_active"))
            device_ids = [str(r[0]) for r in rows]
        for device_id in device_ids:
            try:
                await _generate_for_device(engine, device_id, target_date)
            except Exception as e:
                logger.exception(f"Diary generation failed for {device_id[:8]}: {e}")
    finally:
        await engine.dispose()


async def _generate_for_device(engine, device_id: str, target_date: date) -> None:
    start, end = diary_window_utc(target_date)
    start_3d = start - timedelta(days=2)

    # ── 1. 互动计数 (阈值判断) ──
    async with engine.connect() as conn:
        user_cnt = (
            await conn.execute(
                text("SELECT COUNT(*) FROM conversations WHERE device_id=:d AND role='user' "
                     "AND created_at >= :s AND created_at < :e"),
                {"d": device_id, "s": start, "e": end},
            )
        ).scalar() or 0
        ev_cnt = (
            await conn.execute(
                text("SELECT COUNT(*) FROM sensor_events WHERE device_id=:d "
                     "AND event_type IN ('shake','tap','petting','voice') "
                     "AND created_at >= :s AND created_at < :e"),
                {"d": device_id, "s": start, "e": end},
            )
        ).scalar() or 0
        interaction_count = int(user_cnt) + int(ev_cnt)
        if interaction_count < settings.diary_min_interactions:
            logger.info(f"[{device_id[:8]}] interactions {interaction_count} < "
                        f"{settings.diary_min_interactions} — skip")
            return

        # ── 2. 素材: 近三天对话 (带上海时间戳拼行) ──
        rows = (
            await conn.execute(
                text("SELECT role, content, created_at FROM conversations "
                     "WHERE device_id=:d AND role IN ('user','assistant') "
                     "AND created_at >= :s AND created_at < :e ORDER BY created_at"),
                {"d": device_id, "s": start_3d, "e": end},
            )
        ).all()
        lines = []
        for role, content, created_at in rows:
            ts = created_at.astimezone(timezone.utc) + SHIFT
            who = "主人" if role == "user" else "萝莉丝"
            lines.append(f"[{ts:%m-%d %H:%M}] {who}: {content}")
        dialogue_text = "\n".join(lines)[:MAX_DIALOGUE_CHARS]

        # ── 3. 素材: 重要历史 (device_memories) ──
        mem = (
            await conn.execute(
                text("SELECT content FROM device_memories WHERE device_id=:d"),
                {"d": device_id},
            )
        ).scalar()
        history_text = mem or "(无)"

        # ── 4. 素材: 当日互动事件 ──
        ev_rows = (
            await conn.execute(
                text("SELECT event_type, created_at FROM sensor_events "
                     "WHERE device_id=:d AND created_at >= :s AND created_at < :e "
                     "ORDER BY created_at"),
                {"d": device_id, "s": start, "e": end},
            )
        ).all()
        event_lines = []
        for event_type, created_at in ev_rows:
            ts = created_at.astimezone(timezone.utc) + SHIFT
            event_lines.append(f"[{ts:%m-%d %H:%M}] 互动: {EVENT_LABELS.get(event_type, event_type)}")
        event_text = "\n".join(event_lines) or "(无)"

    material = (
        f"【目标日期】{target_date:%Y年%m月%d日} (当天互动次数: {interaction_count})\n"
        f"【重要历史】\n{history_text}\n"
        f"【当日互动】\n{event_text}\n"
        f"【对话记录 (含前后两天, 仅作上下文)】\n{dialogue_text}"
    )

    # ── 5. LLM 生成 ──
    from app.utils.prompt_templates import DIARY_GENERATION_PROMPT
    from app.services.llm_service import chat_json
    result = await chat_json(
        DIARY_GENERATION_PROMPT, material, max_tokens=800, temperature=0.8
    )
    title, content, mood, fallback_used = _coerce_result(
        result, dialogue_text, target_date
    )

    # ── 6. Upsert (device_id+entry_date 唯一) ──
    now = datetime.now(timezone.utc)
    entry_id = str(uuid.uuid4())
    async with engine.begin() as conn:
        existing = (
            await conn.execute(
                text("SELECT id FROM diary_entries WHERE device_id=:d AND entry_date=:dt"),
                {"d": device_id, "dt": target_date},
            )
        ).fetchone()
        if existing:
            entry_id = str(existing[0])
            await conn.execute(
                text("UPDATE diary_entries SET title=:t, content=:c, mood_summary=:m, "
                     "interaction_count=:ic, doodle_url=NULL WHERE id=:id"),
                {"t": title, "c": content, "m": mood, "ic": interaction_count,
                 "id": existing[0]},
            )
        else:
            await conn.execute(
                text("INSERT INTO diary_entries (id, device_id, entry_date, title, content, "
                     "mood_summary, interaction_count, created_at) "
                     "VALUES (:id, :d, :dt, :t, :c, :m, :ic, :ts)"),
                {"id": entry_id, "d": device_id, "dt": target_date,
                 "t": title, "c": content, "m": mood, "ic": interaction_count, "ts": now},
            )

    # ── 7. 涂鸦 (小概率 25%, 失败不阻断; LLM 兜底路径跳过) ──
    if not fallback_used:
        try:
            from app.services.doodle_service import maybe_generate_doodle
            url = await maybe_generate_doodle(entry_id, title, content)
            if url:
                async with engine.begin() as conn:
                    await conn.execute(
                        text("UPDATE diary_entries SET doodle_url=:u WHERE id=:id"),
                        {"u": url, "id": entry_id},
                    )
        except Exception as e:
            logger.warning(f"Doodle step failed: {e}")

    logger.info(f"[{device_id[:8]}] diary {target_date} saved "
                f"(title={title[:20]!r}, interactions={interaction_count}, "
                f"fallback={fallback_used})")


def _coerce_result(result, dialogue_text: str, target_date: date):
    """LLM 输出校验与兜底 — 非 dict/含 _raw/字段缺失 → 模板标题 + 对话前 100 字.
    返回 (title, content, mood_summary, fallback_used)."""
    ok = isinstance(result, dict) and "_raw" not in result
    title = (result.get("title", "").strip() if ok else "")
    content = (result.get("content", "").strip() if ok else "")
    mood = (result.get("mood_summary", "").strip() if ok else "")
    fallback_used = not (title and content and len(content) >= 20)
    if fallback_used:
        logger.warning("Diary LLM output invalid — using fallback template")
        title = f"{target_date:%m月%d日} 的一天"
        content = dialogue_text.strip()[:100] or "今天和主人一起度过了平凡的一天。"
        mood = ""
    return title[:20], content[:600], mood[:4], fallback_used
