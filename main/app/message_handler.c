/**
 * @file message_handler.c
 * @brief 协议帧语义层 — 帧分发
 *
 * 迁移路线 (与 ws_client 默认链删分支同 commit 闭合, 逐位可对账):
 *   ④-4 ✅ get_memory / memory_update 已迁入
 *   ④-5  chat_text / chat_done 迁入 (chat_seq 落地 + parse_reply_text + 动画)
 *   ④-6  scan_wifi 迁入
 *
 * 已认领的帧返回 true; 未认领的返回 false → ws_client 走内部默认链
 * (audio 起止等传输耦合物, 留在传输层)。
 */
#include "message_handler.h"
#include "ws_client.h"
#include "memory_store.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "msg";

/* get_memory 读盘结果回调 — 写盘任务上下文执行 (仅 socket 发送, 零 flash 访问):
 * flash 读期间 cache 禁用, WS 任务栈在 PSRAM, 任何 flash 访问都会
 * double exception */
static void msg_memory_read_cb(const char *content, size_t len, void *arg)
{
    char *rid = (char *)arg;
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "memory_data");
    if (rid && rid[0]) cJSON_AddStringToObject(resp, "req_id", rid);
    cJSON_AddNumberToObject(resp, "size", (double)len);
    cJSON_AddStringToObject(resp, "content", content ? content : "");
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (json) {
        ESP_LOGI(TAG, "memory_data: %u B (req=%s)", (unsigned)len, rid ? rid : "?");
        ws_client_send_json(json);
        cJSON_free(json);
    }
    free(rid); /* 调用方 strdup 的 req_id, 回调上下文释放 */
}

bool message_handler_handle_frame(const char *type, cJSON *root)
{
    /* ── get_memory: 服务端工具 /tools.history 拉取完整对话记忆 ── */
    if (strcmp(type, "get_memory") == 0) {
        /* 读盘委托写盘任务 (回调在写盘任务上下文) — WS 任务栈在
         * PSRAM, cache 禁用期对 flash 的 stat/fopen 读会
         * double exception */
        cJSON *req_id = cJSON_GetObjectItem(root, "req_id");
        char *rid = NULL;
        if (cJSON_IsString(req_id) && req_id->valuestring[0])
            rid = strdup(req_id->valuestring); /* 回调上下文释放 */
        if (memory_store_read_async(msg_memory_read_cb, rid) != ESP_OK) {
            free(rid);
            /* 入队失败仍要应答, 否则服务端工具调用挂起 */
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddStringToObject(resp, "type", "memory_data");
            if (cJSON_IsString(req_id))
                cJSON_AddStringToObject(resp, "req_id", req_id->valuestring);
            cJSON_AddNumberToObject(resp, "size", 0);
            cJSON_AddStringToObject(resp, "content", "");
            char *json = cJSON_PrintUnformatted(resp);
            cJSON_Delete(resp);
            if (json) {
                ws_client_send_json(json);
                cJSON_free(json);
            }
        }
        return true;
    }
    /* ── memory_update: 服务端压缩后下发覆盖 ── */
    if (strcmp(type, "memory_update") == 0) {
        cJSON *content = cJSON_GetObjectItem(root, "content");
        if (cJSON_IsString(content))
            memory_store_overwrite(content->valuestring);
        return true;
    }
    /* ④-5: chat_text / chat_done 迁入这里 */
    /* ④-6: scan_wifi 迁入这里 */
    return false; /* 未认领 → ws_client 默认链 */
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