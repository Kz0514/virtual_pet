"""TTS endpoints — raw PCM from Qwen-Audio-TTS, with streaming support."""
import logging
from fastapi import APIRouter, Depends, HTTPException, Query
from fastapi.responses import Response, StreamingResponse
from app.api.deps import get_current_device
from app.models import Device
from app.services.tts_service import synthesize, synthesize_stream

logger = logging.getLogger("tts_api")
router = APIRouter()


@router.post("/synthesize")
async def tts_synthesize(
    text: str = Query(..., min_length=1, max_length=200),
    voice: str = Query(None),   # 不传则使用服务器默认音色 (DASHSCOPE_VOICE_ID)
    instruction: str = Query(None, max_length=500),
    device: Device = Depends(get_current_device),
):
    """Returns raw PCM 48kHz mono 16-bit audio."""
    # cosyvoice 对≤2字文本会 "no data timeout" — 直接返回空音频, 设备静默跳过
    if len(text.strip()) <= 2:
        logger.info(f"TTS skip (text too short): '{text}'")
        return Response(content=b"", media_type="audio/pcm",
                        headers={"X-Sample-Rate": "48000"})

    audio = await synthesize(text, voice, instruction)
    if audio is None:
        raise HTTPException(500, "TTS synthesis failed")

    return Response(content=audio, media_type="audio/pcm",
                    headers={"X-Sample-Rate": "48000"})


@router.post("/synthesize-stream")
async def tts_synthesize_stream(
    text: str = Query(..., min_length=1, max_length=200),
    voice: str = Query(None),   # 不传则使用服务器默认音色
    device: Device = Depends(get_current_device),
):
    """Streams raw PCM 48kHz mono 16-bit audio as it's generated.
       Uses chunked transfer encoding — client must decode chunks."""
    return StreamingResponse(
        synthesize_stream(text, voice),
        media_type="audio/pcm",
        headers={"X-Sample-Rate": "48000", "Cache-Control": "no-cache"},
    )


