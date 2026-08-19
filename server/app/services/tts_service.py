"""Qwen-Audio-TTS — returns raw PCM 48kHz mono 16-bit. Supports streaming."""
import asyncio, dashscope, logging, queue, threading, time
from dashscope.audio.tts_v2 import SpeechSynthesizer, AudioFormat, ResultCallback
from app.config import get_settings

settings = get_settings()
logger = logging.getLogger("tts_service")


async def synthesize(text: str, voice: str = None,
                     instruction: str = None) -> bytes | None:
    if voice is None:
        voice = settings.dashscope_voice_id
    """Non-streaming TTS, returns full PCM bytes. instruction = natural language voice control."""
    if not settings.dashscope_api_key:
        logger.error("DashScope API key not configured")
        return None

    dashscope.api_key = settings.dashscope_api_key

    try:
        kwargs = dict(model="qwen-audio-3.0-tts-flash", voice=voice,
                      format=AudioFormat.PCM_48000HZ_MONO_16BIT)
        if instruction:
            kwargs["instruction"] = instruction
            logger.info(f"TTS inst: {instruction[:80]}")
        synthesizer = SpeechSynthesizer(**kwargs)
        loop = asyncio.get_running_loop()
        audio = await loop.run_in_executor(None, synthesizer.call, text)
        if audio:
            logger.info(f"TTS: {len(audio)} bytes PCM 48kHz")
        return audio
    except Exception as e:
        logger.error(f"TTS failed: {e}")
        return None


async def synthesize_stream(text: str, voice: str = None):
    if voice is None:
        voice = settings.dashscope_voice_id
    """Async generator: yields PCM chunks as TTS generates them.
       Uses streaming_call with ResultCallback, bridged via thread-safe Queue."""
    if not settings.dashscope_api_key:
        logger.error("DashScope API key not configured")
        return

    dashscope.api_key = settings.dashscope_api_key

    q: queue.Queue = queue.Queue(maxsize=512)

    class StreamCallback(ResultCallback):
        def on_data(self, data: bytes) -> None:
            try:
                # 背压而非丢块 — 队列满说明网络发送落后于合成速度,
                # 短暂阻塞合成线程, 丢块会造成设备端可闻卡顿/跳字
                q.put(data, timeout=5)
            except queue.Full:
                logger.warning("TTS stream queue full after 5s, dropping chunk")
        def on_complete(self) -> None:
            try:
                q.put(None, timeout=5)   # sentinel
            except queue.Full:
                pass
        def on_error(self, message: str) -> None:
            logger.error(f"TTS stream error: {message}")
            try:
                q.put(None, timeout=5)
            except queue.Full:
                pass
        def on_close(self) -> None:
            pass

    callback = StreamCallback()
    synthesizer = SpeechSynthesizer(
        model="qwen-audio-3.0-tts-flash",
        voice=voice,
        format=AudioFormat.PCM_48000HZ_MONO_16BIT,
        callback=callback,
    )

    loop = asyncio.get_running_loop()

    def _run():
        try:
            synthesizer.call(text)
        except Exception as e:
            logger.error(f"TTS stream call failed: {e}")
            q.put(None)

    # Run blocking call() in thread pool — doesn't block event loop
    task = loop.run_in_executor(None, _run)

    # 匀速发送: 合成是突发的(快一阵停一阵), 直接透传会让设备端环形缓冲
    # 过冲/欠冲造成卡顿。这里保持 0.5s 音频预冲, 之后按 1.0x 实时速率
    # (96KB/s @48k*2B) 匀速吐出 — 注意实测合成速率常只有 1.2-2x 实时,
    # 预冲设太大(如 2s)节流永远不触发, 突发会原样透传造成欠冲。
    # 注: 合成本身慢于实时时无法节流挽救, 由设备端缓冲 + 诊断日志兜底。
    BYTES_PER_SEC = 48000 * 2
    LOOKAHEAD_S = 0.5
    start_t = time.monotonic()
    total = 0
    while True:
        chunk = await loop.run_in_executor(None, q.get)
        if chunk is None:
            break
        total += len(chunk)
        ahead = total / BYTES_PER_SEC - (time.monotonic() - start_t)
        if ahead > LOOKAHEAD_S:
            await asyncio.sleep(ahead - LOOKAHEAD_S)
        yield chunk

    await task  # ensure synthesis finished cleanly
    logger.info(f"TTS stream: {total} bytes PCM 48kHz")


