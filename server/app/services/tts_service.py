"""Qwen-Audio-TTS — returns raw PCM 48kHz mono 16-bit. Supports streaming."""
import asyncio, dashscope, logging, queue, re, threading, time
from dashscope.audio.tts_v2 import SpeechSynthesizer, AudioFormat, ResultCallback
from app.config import get_settings

settings = get_settings()
logger = logging.getLogger("tts_service")

# 剥离（动作提示）— Qwen 会把括号内文字也朗读出来 (1.0.257 实测:
# 带提示句的音频时长 = 文本期望 2.1~2.75×, 提示语被完整念出 =
# 用户听到的"复播句首")。提示词已约束模型少用, 此处兜底:
# 全角/半角括号内的内容剥掉, 未闭合的全角括号尾巴也剥掉。
_STAGE_DIR_RE = re.compile(r"[（(][^（()）]*[）)]")


def _strip_stage_directions(text: str) -> str:
    text = _STAGE_DIR_RE.sub("", text)
    return re.sub(r"[（(][^（()）]*$", "", text).strip()


async def synthesize(text: str, voice: str = None,
                     instruction: str = None) -> bytes | None:
    if voice is None:
        voice = settings.dashscope_voice_id
    """Non-streaming TTS, returns full PCM bytes. instruction = natural language voice control."""
    if not settings.dashscope_api_key:
        logger.error("DashScope API key not configured")
        return None

    dashscope.api_key = settings.dashscope_api_key

    text = _strip_stage_directions(text)

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
    text = _strip_stage_directions(text)

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

    # 首块攒够 0.5s 预冲后透传 (2026-08-24 修复): 旧版按 1.0x 实时速率匀速
    # 节流, 实测合成速率仅 1.06x 实时 — 节流把设备端缓冲锁死在 0.5s,
    # 设备网络一抖(实测 DL rate 3~157KB/s 波动)即欠冲卡顿; 且设备端
    # 槽容量有限, 长句音频 > 槽容量时下载任务在 rb_put 满等, 播放一旦
    # 停顿即永久挂死。改为攒够预冲后原速透传: 设备端可积累
    # "合成领先量(6%) + 预冲" 的缓冲, 对抖动容忍度大幅提升。
    BYTES_PER_SEC = 48000 * 2
    PREFILL_S = 0.5
    prefill = b""
    prefilled = False
    total = 0
    while True:
        chunk = await loop.run_in_executor(None, q.get)
        if chunk is None:
            if prefill:
                yield prefill
            break
        total += len(chunk)
        if not prefilled:
            prefill += chunk
            if len(prefill) >= BYTES_PER_SEC * PREFILL_S:
                prefilled = True
                yield prefill
        else:
            yield chunk

    await task  # ensure synthesis finished cleanly
    logger.info(f"TTS stream: {total} bytes PCM 48kHz")


class TTSSession:
    """流式输入会话: feed() 逐句喂文本 → streaming_call 增量合成,
    音频经队列出, WS 推流任务边取边推. feed/finish 同步阻塞 —
    必须 run_in_executor 执行."""

    def __init__(self, voice: str | None = None):
        if voice is None:
            voice = settings.dashscope_voice_id
        self.q: queue.Queue = queue.Queue(maxsize=512)
        self.error: str | None = None
        self._synth: SpeechSynthesizer | None = None

        if not settings.dashscope_api_key:
            logger.error("DashScope API key not configured")
            self.error = "DashScope API key not configured"
            return

        # SpeechSynthesizer 构造即校验 dashscope.api_key, 必须创建前设好
        # (1.0.258 漏设 → 构造异常在 router try 外 → 整轮对话零回执)
        dashscope.api_key = settings.dashscope_api_key

        class _CB(ResultCallback):
            def __init__(self, sess):
                self.sess = sess

            def on_data(self, data: bytes) -> None:
                try:
                    self.sess.q.put(data, timeout=5)
                except queue.Full:
                    logger.warning("TTS session queue full, dropping chunk")

            def on_complete(self) -> None:
                try:
                    self.sess.q.put(None, timeout=5)
                except queue.Full:
                    pass

            def on_error(self, message: str) -> None:
                logger.error(f"TTS session error: {message}")
                self.sess.error = message
                try:
                    self.sess.q.put(None, timeout=5)
                except queue.Full:
                    pass

            def on_close(self) -> None:
                pass

        self._synth = SpeechSynthesizer(
            model="qwen-audio-3.0-tts-flash",
            voice=voice,
            format=AudioFormat.PCM_48000HZ_MONO_16BIT,
            callback=_CB(self),
        )

    def feed(self, text: str) -> None:
        text = _strip_stage_directions(text).strip()
        if not text or self.error:
            return
        try:
            self._synth.streaming_call(text)
        except Exception as e:
            logger.error(f"TTS session feed failed: {e}")
            self.error = str(e)
            try:
                self.q.put(None, timeout=1)
            except queue.Full:
                pass

    def finish(self) -> None:
        """结束合成并收尾. _synth 缺失 (构造失败) 也必须补哨兵 —
        否则推流任务 q.get 永不返回, wait_for 干等 120s 拖死 chat_done."""
        if not self._synth:
            try:
                self.q.put(None, timeout=1)
            except queue.Full:
                pass
            return
        try:
            self._synth.streaming_complete()
        except Exception as e:
            logger.error(f"TTS session complete failed: {e}")
            if not self.error:
                self.error = str(e)
            try:
                self.q.put(None, timeout=1)
            except queue.Full:
                pass


