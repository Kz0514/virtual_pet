/**
 * ASR client — 上传 PCM 到服务器识别为文本 (HTTP multipart)。
 * 原 voice_chat 单按录音通路 (record_and_asr/task) 已被会话模式的
 * VAD 录音取代 (session_mgr), 旧通路连带 init/record_and_asr/task
 * 随 ④-1 删除 — 本接口只保留活跃面。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/** 上传 PCM (48kHz mono 16bit) 到 ASR, 返回识别文本 (caller 需 free)。
 *  供会话模式 VAD 录音复用 — 只传人声段。 */
char *asr_transcribe_pcm(const int16_t *pcm, uint32_t sample_count);

/** 是否正在录音 (噪声检测避让采样) */
bool asr_is_recording(void);