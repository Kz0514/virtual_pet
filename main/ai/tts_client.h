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
/** 流式句子入口 (chat_text 帧, P1 延迟优化): 入队待播, 链空闲则起链 —
 *  链式播放下永不拒绝 (多句组成一轮回复, 逐句并入当前链) */
bool tts_speak_queue(const char *text);
/** 句子队列是否为空 (chat_done 到达时据此判断链是否已播完 — 防漏播兜底) */
bool tts_client_queue_empty(void);
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

/** ── WS 音频流模式: 服务器 LLM 流式期间增量合成,
 *  音频经 WS 二进制帧直推, 设备入环直播 — 无 POST 无下载任务 ── */
/** audio_start: 起播放链 (预冲等 WS 音频), 打断旧链. 返回是否成功 */
bool tts_client_ws_start(void);
/** WS 二进制帧入口 (esp_websocket_client 组件任务上下文 — 零阻塞,
 *  环满丢块计数, 绝不阻塞组件任务导致 ping 超时断连) */
bool tts_client_ws_feed(const uint8_t *data, uint32_t len);
/** audio_end: 置 rb_done — 播放排空收尾 */
void tts_client_ws_end(void);
/** WS 音频流模式是否激活 (chat_done 到达时据此判断是否已流式推送过) */
bool tts_client_is_ws_active(void);

#ifdef __cplusplus
}
#endif
