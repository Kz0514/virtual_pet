"""
ASR endpoint — ESP32 uploads audio → Qwen3-ASR-Flash → text + emotion.
No files saved to disk.
"""
import logging
from fastapi import APIRouter, UploadFile, File, HTTPException, Depends
from app.api.deps import get_current_device
from app.models import Device
from app.services.asr_service import transcribe_base64

logger = logging.getLogger("asr_api")
router = APIRouter()


@router.post("/transcribe")
async def transcribe_audio(
    file: UploadFile = File(...),
    device: Device = Depends(get_current_device),
):
    """Receive audio from ESP32, transcribe with Qwen3-ASR-Flash."""
    if not file.filename:
        raise HTTPException(400, "No filename")

    allowed = {".wav", ".mp3", ".m4a", ".aac", ".ogg", ".opus", ".pcm", ".raw"}
    ext = file.filename.rsplit(".", 1)[-1].lower() if "." in file.filename else "pcm"
    if f".{ext}" not in allowed:
        raise HTTPException(400, f"Unsupported: {ext}")

    content = await file.read()
    if len(content) > 10 * 1024 * 1024:
        raise HTTPException(413, "Audio too large (max 10MB)")

    mime = {
        "wav": "audio/wav", "mp3": "audio/mpeg", "m4a": "audio/mp4",
        "aac": "audio/aac", "ogg": "audio/ogg", "opus": "audio/opus",
        "pcm": "audio/pcm", "raw": "audio/pcm",
    }.get(ext, "audio/wav")

    result = await transcribe_base64(content, mime)
    if result is None:
        raise HTTPException(500, "ASR transcription failed")

    return {"text": result["text"], "emotion": result.get("emotion", "neutral")}


@router.get("/health")
async def asr_health():
    return {"status": "ok", "model": "qwen3-asr-flash"}


