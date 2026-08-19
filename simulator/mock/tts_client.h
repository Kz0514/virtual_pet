/** @file mock/tts_client.h — Mock TTS 客户端 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 模拟器无音频: 不播放/不下载/不忙 → chat_bubble 走"兜底立即显示",
 * pet_avatar 的 TTS 联动逻辑直接跳过 */
static inline void tts_client_init(void) { /* no-op */ }
static inline bool tts_speak(const char *text) { (void)text; return false; }
static inline bool tts_speak_inst(const char *text, const char *instruction) {
    (void)text; (void)instruction; return false;
}
static inline bool tts_client_is_playing(void) { return false; }
static inline bool tts_client_is_downloading(void) { return false; }
static inline uint32_t tts_client_get_playback_ms(void) { return 0; }
static inline bool tts_client_is_busy(void) { return false; }

#ifdef __cplusplus
}
#endif
