/** @file tts_client.h @brief TTS 客户端 — 下载 WAV 并播放 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 TTS (需 WiFi 已连 + 已认证) */
void tts_client_init(void);

/** 合成并播放文本 (异步, 在独立任务中执行)。
 *  忙碌时拒绝 (返回 false) — 要打断请用 tts_client_interrupt_speak */
bool tts_speak(const char *text);
/** 合成并播放文本, 带语音指令控制 (instruction 可为 NULL) */
bool tts_speak_inst(const char *text, const char *instruction);

/** 停止当前播放/下载 (协作式, 立即返回; 无活动返回 false) */
bool tts_client_stop(void);

/** 打断当前播放并立即开始新文本 — 新回复打断旧回复的唯一入口.
 *  内部: 停旧任务 (≤600ms 等待, 正常路径不杀任务) → 复位环形缓冲 → 起新任务 */
bool tts_client_interrupt_speak(const char *text);

/** 检查是否正在播放 (噪音检测器需要避开) */
bool tts_client_is_playing(void);

/** 是否正在下载音频 (动画帧加载需等下载完成, 避免SPI总线争用) */
bool tts_client_is_downloading(void);

/** 播放已开始的毫秒数 (0=未开始) — 供文字同步显示 */
uint32_t tts_client_get_playback_ms(void);

/** TTS全周期是否忙碌 (下载+播放) — 供气泡兜底判断 */
bool tts_client_is_busy(void);

#ifdef __cplusplus
}
#endif
