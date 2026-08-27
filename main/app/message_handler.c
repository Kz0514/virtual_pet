/**
 * @file message_handler.c
 * @brief 协议帧语义层 — 帧分发骨架
 *
 * 迁移路线 (与 ws_client 默认链删分支同 commit 闭合, 逐位可对账):
 *   ④-4  get_memory / memory_update 迁入
 *   ④-5  chat_text / chat_done 迁入 (chat_seq 落地 + parse_reply_text + 动画)
 *   ④-6  scan_wifi 迁入
 *
 * 本步 (④-3) 骨架: 不认领任何帧 (恒 false) → ws_client 走内部默认链,
 * 行为与重构前逐位一致。
 */
#include "message_handler.h"
#include "ws_client.h"

bool message_handler_handle_frame(const char *type, cJSON *root)
{
    (void)type;
    (void)root;
    /* ④-4: get_memory / memory_update 迁入这里 */
    /* ④-5: chat_text / chat_done 迁入这里 */
    /* ④-6: scan_wifi 迁入这里 */
    return false; /* 骨架: 未认领 → ws_client 默认链照旧 */
}

uint32_t message_handler_get_chat_seq(void)
{
    /* ④-5 回收: chat 分支迁入后改为自持计数 (返回内部 s_chat_seq) */
    return ws_client_get_chat_seq();
}

void message_handler_init(void)
{
    ws_client_set_frame_handler(message_handler_handle_frame);
}