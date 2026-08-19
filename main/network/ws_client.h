/**
 * @file ws_client.h
 * @brief WebSocket 客户端 — 服务器实时通信
 */
#pragma once
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ws_client_connect(const char *token);
bool ws_client_is_connected(void);

/** chat_done 累计计数 — 会话模式等待回复的信号 (变化 = 新回复到达) */
uint32_t ws_client_get_chat_seq(void);
esp_err_t ws_client_send_text(const char *text);
esp_err_t ws_client_send_json(const char *json);

/** 发送 chat 消息 (cJSON 构建, 附带记忆元数据 mem_size/mem_summary) */
esp_err_t ws_client_send_chat(const char *text);

#ifdef __cplusplus
}
#endif
