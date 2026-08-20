"""
WebSocket endpoint — streaming chat + tool support + mood_delta 统一更新.
"""
import json, logging, asyncio
from datetime import datetime, timezone
from fastapi import APIRouter, WebSocket, WebSocketDisconnect, Query
from sqlalchemy import text
from app.config import get_settings
from app.core.database import AsyncSessionLocal
from app.core.security import verify_access_token
from app.services.llm_service import chat_with_tools
from app.services.tool_service import (
    cache_sensor, resolve_scan_future, resolve_memory_future, get_device_location,
)
from app.utils.reply_parser import parse_reply, clean_text

logger = logging.getLogger("ws")
settings = get_settings()
router = APIRouter()

# ── Device connection tracking (for async tool → ESP32 communication) ──
device_connections: dict[str, WebSocket] = {}

# ── Per-device chat lock: 串行化同设备并发的 LLM 调用与落库顺序
#    (历史注入后, 并发 chat 会打乱 conversations 顺序). 不随断开清理 —
#    可能有 handle_chat 仍持有旧锁, pop 反而制造新旧锁并存窗口.
_device_locks: dict[str, asyncio.Lock] = {}


def get_device_lock(device_id: str) -> asyncio.Lock:
    lock = _device_locks.get(device_id)
    if lock is None:
        lock = asyncio.Lock()
        _device_locks[device_id] = lock
    return lock


async def send_to_device(device_id: str, message: dict):
    """Send a WS message to a connected ESP32 device."""
    ws = device_connections.get(device_id)
    if ws:
        try:
            await ws.send_json(message)
            return True
        except Exception:
            pass
    return False


def is_device_connected(device_id: str) -> bool:
    """Check if an ESP32 device is currently connected via WS."""
    return device_id in device_connections


@router.websocket("/device")
async def device_ws(websocket: WebSocket, token: str = Query("")):
    logger.info(f"WS token: len={len(token)} first={token[:30]} last={token[-20:]}")
    device_id = None
    if token:
        payload = verify_access_token(token)
        if payload:
            device_id = payload.get("sub")
        else:
            logger.warning(f"WS verify FAILED len={len(token)}")

    await websocket.accept()

    if device_id is None:
        await websocket.send_json({"type": "error", "message": "未授权"})
        await websocket.close()
        return

    # Track connection for async tool communication
    device_connections[device_id] = websocket
    await websocket.send_json({"type": "hello", "message": "连接成功", "device_id": device_id})

    try:
        while True:
            data = await websocket.receive_text()
            try:
                msg = json.loads(data)
            except json.JSONDecodeError:
                await websocket.send_json({"type": "error", "message": "无效 JSON"})
                continue

            msg_type = msg.get("type", "")

            if msg_type == "ping":
                await websocket.send_json({"type": "pong"})

            elif msg_type == "chat":
                user_text = msg.get("text", "")
                if not user_text:
                    await websocket.send_json({"type": "error", "message": "消息为空"})
                    continue

                context = msg.get("context")
                if context and device_id:
                    cache_sensor(device_id, context)
                mem_summary = msg.get("mem_summary", "")
                mem_size = msg.get("mem_size", 0)
                if mem_summary:
                    # 重要历史落库 — 压缩态摘要即服务端素材 (覆盖为最新)
                    try:
                        from app.services.memory_service import upsert_device_memory
                        async with AsyncSessionLocal() as db:
                            await upsert_device_memory(db, str(device_id), mem_summary)
                            await db.commit()
                    except Exception as e:
                        logger.warning(f"Memory upsert fail: {e}")

                async def _chat_impl():
                    """Process chat in background so WS loop can handle scan_result etc."""
                    nonlocal user_text, context, mem_summary, mem_size
                    pet_state = ""
                    try:
                        async with AsyncSessionLocal() as db:
                            r = await db.execute(
                                text("SELECT mood, intimacy, name FROM pets WHERE device_id=:d"),
                                {"d": device_id}
                            )
                            row = r.fetchone()
                            if row:
                                mood_labels = {range(0,20):"极度低落", range(20,40):"有点难过",
                                               range(40,60):"平静", range(60,80):"开心", range(80,101):"超级兴奋"}
                                ml = "平静"
                                for rng, label in mood_labels.items():
                                    if row[0] in rng: ml = label; break
                                pet_state = f"[{row[2]} 心情{row[0]}/100({ml}) 亲密度{row[1]}/100]"
                    except Exception:
                        pass
                    loc = get_device_location(device_id)
                    if loc:
                        src = loc.get("source", "unknown")
                        if src == "ip":
                            # 实测: LLM 总把 IP 定位的城市中心坐标当实际坐标 — 明示粗略
                            pet_state += (f" | 设备位置:{loc.get('province','')}{loc.get('city','')} "
                                          f"坐标:{loc.get('lat',0):.3f},{loc.get('lng',0):.3f}"
                                          f"(注意:这是IP定位的城市中心粗略坐标, 误差数公里, "
                                          f"不是设备实际位置, 不要当精确坐标使用)")
                        else:
                            acc = f"约{loc.get('accuracy',0)}米"
                            pet_state += f" | 设备位置:{loc.get('province','')}{loc.get('city','')} 坐标:{loc.get('lat',0):.3f},{loc.get('lng',0):.3f} 精度:{acc}(来自{src})"

                    if mem_summary:
                        pet_state += f"\n[宠物记忆摘要]\n{mem_summary}"
                    if mem_size > settings.memory_max_bytes:
                        logger.info(f"Device memory {mem_size}B over limit — "
                                    f"hint LLM to call /tools.history for compaction")

                    # 加载最近 N 轮历史 (chat_with_tools 自己会 append 本轮 user_text)
                    history = []
                    try:
                        async with AsyncSessionLocal() as db:
                            r = await db.execute(
                                text("SELECT role, content FROM conversations "
                                     "WHERE device_id=:d AND role IN ('user','assistant') "
                                     "ORDER BY created_at DESC LIMIT :n"),
                                {"d": device_id, "n": settings.conversation_window_size},
                            )
                            rows = r.fetchall()
                            history = [
                                {"role": row[0],
                                 "content": clean_text(row[1]) if row[0] == "assistant" else row[1]}
                                for row in reversed(rows)
                            ]
                    except Exception as e:
                        logger.warning(f"History load fail: {e}")

                    # 本轮 user 行先落库 (失败不阻断对话)
                    try:
                        async with AsyncSessionLocal() as db:
                            await db.execute(
                                text("INSERT INTO conversations (device_id,role,content,created_at) "
                                     "VALUES (:d,'user',:c,:t)"),
                                {"d": device_id, "c": user_text, "t": datetime.now(timezone.utc)},
                            )
                            await db.commit()
                    except Exception as e:
                        logger.warning(f"User save fail: {e}")

                    reply = ""
                    try:
                        reply, tools_used = await chat_with_tools(
                            user_text=user_text,
                            history=history,
                            device_id=device_id,
                            extra_context=pet_state,
                        )
                    except Exception as e:
                        logger.error(f"LLM error: {e}")
                        reply = f"萝莉丝睡着了...({str(e)[:50]})"

                    # Parse model output & send reply
                    stripped = reply.strip()
                    logger.info(f"RAW reply ({len(stripped)}): {stripped[:200]}")
                    chat_text, meta = parse_reply(reply)

                    mood_delta = meta.get("mood_delta", 0)
                    try:
                        mood_delta = max(-3, min(10, int(mood_delta)))
                    except (ValueError, TypeError):
                        mood_delta = 0
                    exp = meta.get("exp", 0)
                    try:
                        exp = max(-1, min(5, int(exp)))
                    except (ValueError, TypeError):
                        exp = 0
                    logger.info(f"Parsed: mood_delta={mood_delta:+d} exp={exp:+d} text={chat_text[:50]}")

                    try:
                        await websocket.send_json({
                            "type": "chat_done",
                            "text": chat_text.strip(),
                            "user_text": user_text,
                            "mood_delta": mood_delta,
                            "exp": exp,
                            "animation": meta.get("animation", "none"),
                        })
                    except Exception as e:
                        logger.warning(f"chat_done send fail: {e}")

                    try:
                        async with AsyncSessionLocal() as db:
                            now = datetime.now(timezone.utc)
                            await db.execute(
                                text("UPDATE pets SET mood=LEAST(100,GREATEST(0,mood+:d)) "
                                     "WHERE device_id=:did"),
                                {"d": mood_delta, "did": device_id},
                            )
                            await db.execute(
                                text("INSERT INTO conversations (device_id,role,content,created_at) "
                                     "VALUES (:d,'assistant',:c,:t)"),
                                {"d": device_id, "c": clean_text(chat_text), "t": now},
                            )
                            await db.commit()
                    except Exception as e:
                        logger.warning(f"Save fail: {e}")

                async def handle_chat():
                    """同设备串行锁 — 并发 chat 会打乱历史与落库顺序."""
                    lock = get_device_lock(device_id)
                    await lock.acquire()
                    try:
                        await _chat_impl()
                    finally:
                        lock.release()

                # Spawn chat as background task so WS loop stays responsive
                asyncio.create_task(handle_chat())

            elif msg_type == "scan_result":
                # ESP32 responded to scan_wifi request from tool_network_location
                wifiinfo = msg.get("wifiinfo", [])
                resolve_scan_future(device_id, {"wifiinfo": wifiinfo})
                await websocket.send_json({"type": "scan_ack"})

            elif msg_type == "memory_data":
                # ESP32 responded to get_memory request from tool_history
                resolve_memory_future(msg.get("req_id", ""), msg)
                await websocket.send_json({"type": "memory_ack"})

            elif msg_type == "sensor_data":
                sensor_data = msg.get("data", {})
                if sensor_data and device_id:
                    cache_sensor(device_id, sensor_data)
                    await websocket.send_json({"type": "sensor_ack"})

            elif msg_type == "sensor_event":
                event_type = msg.get("event", "")
                event_data = msg.get("data", {})
                try:
                    async with AsyncSessionLocal() as db:
                        await db.execute(
                            text("INSERT INTO sensor_events (device_id,event_type,event_data,created_at) VALUES (:d,:e,:ed,:t)"),
                            {"d": device_id, "e": event_type, "ed": json.dumps(event_data),
                             "t": datetime.now(timezone.utc)},
                        )
                        await db.commit()
                except Exception as e:
                    logger.warning(f"Sensor save fail: {e}")

            else:
                await websocket.send_json({"type": "error", "message": f"未知类型: {msg_type}"})

    except WebSocketDisconnect:
        logger.info(f"Device {device_id} disconnected")
        # 只移除自己的连接 — 防止旧连接断开误删新连接 (重连竞态)
        if device_connections.get(device_id) is websocket:
            device_connections.pop(device_id, None)
    except Exception as e:
        logger.error(f"WS exception: {e}")
        if device_connections.get(device_id) is websocket:
            device_connections.pop(device_id, None)


