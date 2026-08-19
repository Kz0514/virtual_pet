/**
 * @file mock_impl.c
 * @brief 模拟器硬件桩实现 — 供"extern 声明"方式的调用链接
 *
 * 部分 UI 文件 (如 pet_avatar.c) 用 extern 声明直接引用硬件函数,
 * 不包含头文件, 因此需要真实的链接符号 (static inline 头文件无效).
 */
#include <stdbool.h>
#include <stdint.h>

/* ── TTS ── */
void tts_client_init(void) {}
bool tts_speak(const char *text) { (void)text; return false; }
bool tts_speak_inst(const char *text, const char *instruction) {
    (void)text; (void)instruction; return false;
}
bool tts_client_is_playing(void) { return false; }
bool tts_client_is_downloading(void) { return false; }
uint32_t tts_client_get_playback_ms(void) { return 0; }
bool tts_client_is_busy(void) { return false; }

/* ── 触觉马达 ── */
void tm6604_vibrate(uint8_t duty_pct, uint16_t duration_ms) {
    (void)duty_pct; (void)duration_ms;
}

/* ── 手势检测 ── */
void tap_detector_suppress(bool on) { (void)on; }
void shake_detector_suppress(bool on) { (void)on; }
