/**
 * @file ws_client.h
 * @brief WebSocket 客户端 — 服务器实时通信
 */
#pragma once
#include "esp_err.h"
#include "cJSON.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 协议帧回调 — ws_client 解析出 type 后调用 (WS 任务上下文, 同步);
 *  返回 true = 本回调已认领该帧, ws_client 不再走内部默认链。
 *  app/message_handler 注册 (④-3+ 协议层逐步迁入)。 */
typedef bool (*ws_frame_handler_t)(const char *type, cJSON *root);
void ws_client_set_frame_handler(ws_frame_handler_t cb);

esp_err_t ws_client_connect(const char *token);
bool ws_client_is_connected(void);

/** 息屏暂停 (停 WS 组件 + 停止内置重连计时器): WiFi 已停后组件周期重连
 * 必败 (DNS 失败), 息屏期彻底不连。 */
void ws_client_pause(void);

/** 亮屏恢复: 重新启动组件连接 (WiFi 恢复后调用)。 */
void ws_client_resume(void);

/** 本轮 WS 音频流消费接口 — 读出即清零 (message_handler 在 chat_done 用):
 *  true = 本轮收到过 audio_start (音频已 WS 直推, 跳过整段 TTS POST) */
bool ws_audio_stream_take_seen(void);
esp_err_t ws_client_send_text(const char *text);
esp_err_t ws_client_send_json(const char *json);

/** 发送 chat 消息 (cJSON 构建, 附带记忆元数据 mem_size/mem_summary) */
esp_err_t ws_client_send_chat(const char *text);

#ifdef __cplusplus
}
#endif