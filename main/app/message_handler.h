/**
 * @file message_handler.h
 * @brief 协议帧语义层 — WS 帧分发 (ws_client 只做传输)
 */
#pragma once
#include "cJSON.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 注册帧回调 (main.c 调用; 存函数指针, 无先后依赖)。 */
void message_handler_init(void);

/** 协议帧分发入口 — ws_client 解析出 type 后回调; 返回 true = 本层已认领,
 *  ws_client 不再走默认链。 */
bool message_handler_handle_frame(const char *type, cJSON *root);

/** chat_done 累计计数 — 会话模式等待回复的信号 (变化 = 新回复到达)。
 *  ④-5: chat 分支迁入后改为自持计数, 回收桥。 */
uint32_t message_handler_get_chat_seq(void);

#ifdef __cplusplus
}
#endif