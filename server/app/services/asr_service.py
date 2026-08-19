"""
Qwen3-ASR-Flash via DashScope MultiModalConversation API.

Supports: URL / base64 / local file, streaming, emotion detection.
Model: qwen3-asr-flash (5min max, 10MB max)
"""
import logging, asyncio, base64, pathlib
import dashscope
from app.config import get_settings

logger = logging.getLogger("asr")
settings = get_settings()

dashscope.base_http_api_url = settings.dashscope_base_url


async def transcribe(file_path: str, language: str = "zh",
                     enable_itn: bool = False) -> dict | None:
    """
    Transcribe a local audio file (synchronous, non-streaming).

    Returns dict with keys: text, emotion, language, raw
    """
    audio_uri = f"file://{file_path}"

    messages = [{"role": "user", "content": [{"audio": audio_uri}]}]

    loop = asyncio.get_event_loop()
    try:
        response = await loop.run_in_executor(
            None,
            lambda: dashscope.MultiModalConversation.call(
                api_key=settings.dashscope_api_key,
                model="qwen3-asr-flash",
                messages=messages,
                result_format="message",
                asr_options={"language": language, "enable_itn": enable_itn},
            )
        )
    except Exception as e:
        logger.error(f"DashScope call failed: {e}")
        return None

    return _parse_response(response)


async def transcribe_url(audio_url: str, language: str = "zh",
                         enable_itn: bool = False) -> dict | None:
    """
    Transcribe audio from a public URL.
    """
    messages = [{"role": "user", "content": [{"audio": audio_url}]}]

    loop = asyncio.get_event_loop()
    try:
        response = await loop.run_in_executor(
            None,
            lambda: dashscope.MultiModalConversation.call(
                api_key=settings.dashscope_api_key,
                model="qwen3-asr-flash",
                messages=messages,
                result_format="message",
                asr_options={"language": language, "enable_itn": enable_itn},
            )
        )
    except Exception as e:
        logger.error(f"DashScope call failed: {e}")
        return None

    return _parse_response(response)


async def transcribe_base64(audio_bytes: bytes, mime_type: str = "audio/wav",
                            language: str = "zh", enable_itn: bool = False) -> dict | None:
    """
    Transcribe audio from base64-encoded data URI.
    """
    b64 = base64.b64encode(audio_bytes).decode()
    data_uri = f"data:{mime_type};base64,{b64}"

    messages = [{"role": "user", "content": [{"audio": data_uri}]}]

    loop = asyncio.get_event_loop()
    try:
        response = await loop.run_in_executor(
            None,
            lambda: dashscope.MultiModalConversation.call(
                api_key=settings.dashscope_api_key,
                model="qwen3-asr-flash",
                messages=messages,
                result_format="message",
                asr_options={"language": language, "enable_itn": enable_itn},
            )
        )
    except Exception as e:
        logger.error(f"DashScope call failed: {e}")
        return None

    return _parse_response(response)


def _parse_response(response) -> dict | None:
    """Extract text + emotion from DashScope response."""
    try:
        output = response.output
        if output is None:
            logger.error(f"ASR returned no output: {response}")
            return None
        choices = output.get("choices", [])
        if not choices:
            logger.error(f"No choices in response: {output}")
            return None

        message = choices[0].get("message", {})
        content_list = message.get("content", [])

        full_text = ""
        emotion = "neutral"
        for item in content_list:
            if isinstance(item, dict):
                full_text += item.get("text", "")
                annotations = item.get("annotations", [])
                for ann in annotations:
                    if ann.get("type") == "emotion":
                        emotion = ann.get("emotion", "neutral")

        logger.info(f"ASR: '{full_text[:80]}' emotion={emotion}")
        return {"text": full_text.strip(), "emotion": emotion}
    except Exception as e:
        logger.error(f"Parse ASR response failed: {e}")
        return None


# ── Streaming version ──

async def transcribe_stream(file_path: str, language: str = "zh"):
    """
    Streaming transcription — yields partial text dicts as they arrive.
    Each yield: {"text": "...", "is_final": bool}
    """
    audio_uri = f"file://{file_path}"
    messages = [{"role": "user", "content": [{"audio": audio_uri}]}]

    loop = asyncio.get_event_loop()

    def _stream():
        return dashscope.MultiModalConversation.call(
            api_key=settings.dashscope_api_key,
            model="qwen3-asr-flash",
            messages=messages,
            result_format="message",
            asr_options={"language": language, "enable_itn": False},
            stream=True,
        )

    try:
        gen = await loop.run_in_executor(None, _stream)
        for chunk in gen:
            try:
                text = chunk["output"]["choices"][0]["message"]["content"][0]["text"]
                if text:
                    yield {"text": text, "is_final": False}
            except (KeyError, IndexError, TypeError):
                pass
        yield {"text": "", "is_final": True}
    except Exception as e:
        logger.error(f"ASR stream failed: {e}")
        yield {"text": "", "is_final": True, "error": str(e)}


