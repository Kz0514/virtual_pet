/**
 * Voice chat — press-and-hold recording → ASR → LLM via WebSocket.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/** Initialize ES8311 for voice chat (called once at boot) */
void voice_chat_init(void);

/** Record audio, send to ASR server, return text (caller must free).
 *  Returns NULL on failure. */
char *voice_chat_record_and_asr(void);

/** 上传任意 PCM (48kHz mono 16bit) 到 ASR, 返回识别文本 (caller 需 free).
 *  供会话模式 VAD 录音复用 — 只传人声段. */
char *voice_asr_transcribe_pcm(const int16_t *pcm, uint32_t sample_count);

/** FreeRTOS task: init if needed, record, ASR, send to LLM via WS */
void voice_chat_task(void *pvParameter);

/** Check if currently recording (for noise detector to avoid sampling) */
bool voice_chat_is_recording(void);
