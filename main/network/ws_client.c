/**
 * @file ws_client.c
 * @brief WebSocket 客户端 — 服务器实时通信 (仅文本, 不做音频流)
 */
#include "ws_client.h"
#include "server_config.h"
#include "chat_bubble.h"
#include "tts_client.h"
#include "pet_engine.h"
#include "pet_avatar.h"
#include "wifi_scanner.h"
#include "memory_store.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "ws";
static esp_websocket_client_handle_t s_client = NULL;
static bool s_connected = false;
static volatile uint32_t s_chat_seq = 0;   /* chat_done 计数 — 会话模式等待回复信号 */
static char s_last_user_text[512] = {0};   /* 服务端回显的用户原文 — 设备端记忆用 */

/* 分片重组缓冲 (PSRAM) — 大消息 (memory_update 等) 按片到达 */
static char *s_rx_buf = NULL;
static int   s_rx_len = 0, s_rx_total = 0;

uint32_t ws_client_get_chat_seq(void) { return s_chat_seq; }

/* 动画名 → pet_anim_t (PET_ANIM_COUNT = 未识别) */
static pet_anim_t parse_anim_name(const char *a) {
    if      (strcmp(a, "idle") == 0)       return PET_ANIM_IDLE;
    else if (strcmp(a, "happy") == 0)      return PET_ANIM_HAPPY;
    else if (strcmp(a, "sad") == 0)        return PET_ANIM_SAD;
    else if (strcmp(a, "excited") == 0)    return PET_ANIM_EXCITED;
    else if (strcmp(a, "surprised") == 0)  return PET_ANIM_SURPRISED;
    else if (strcmp(a, "sleepy") == 0)     return PET_ANIM_SLEEPY;
    else if (strcmp(a, "eating") == 0)     return PET_ANIM_EATING;
    else if (strcmp(a, "blush") == 0)      return PET_ANIM_BLUSH;
    else if (strcmp(a, "pathead") == 0)    return PET_ANIM_PATHEAD;
    else if (strcmp(a, "scratch") == 0)    return PET_ANIM_SCRATCH;
    else if (strcmp(a, "pointself") == 0)  return PET_ANIM_POINTSELF;
    return PET_ANIM_COUNT;
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    esp_websocket_event_data_t *evt = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WS 已连接! (ws 任务栈水位 %u B)",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
        s_connected = true;
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WS 断开");
        s_connected = false;
        if (s_rx_buf) { free(s_rx_buf); s_rx_buf = NULL; }   /* 丢弃半截消息 */
        s_rx_len = s_rx_total = 0;
        break;
    case WEBSOCKET_EVENT_DATA: {
        /* 分片消息重组: payload_len > data_len 表示本条只是其中一片 */
        const char *msg; int msg_len;
        if (evt->payload_len > evt->data_len) {
            if (!s_rx_buf) {
                s_rx_buf = heap_caps_malloc(evt->payload_len + 1, MALLOC_CAP_SPIRAM);
                if (!s_rx_buf) { ESP_LOGE(TAG, "重组缓冲分配失败"); break; }
                s_rx_len = 0; s_rx_total = evt->payload_len;
            }
            memcpy(s_rx_buf + evt->payload_offset, evt->data_ptr, evt->data_len);
            s_rx_len += evt->data_len;
            if (s_rx_len < s_rx_total) break;      /* 等剩余分片 */
            s_rx_buf[s_rx_len] = '\0';
            msg = s_rx_buf; msg_len = s_rx_len;
            s_rx_buf = NULL; s_rx_len = s_rx_total = 0;
        } else {
            msg = (const char *)evt->data_ptr; msg_len = evt->data_len;
        }
        /* Only log JSON messages (ignore binary/control frames) */
        if (msg_len > 0 && msg[0] == '{')
            ESP_LOGI(TAG, "<<< %.*s", msg_len, msg);
        if (strstr(msg, "\"error\"") ||
            strstr(msg, "未授权")) {
            ESP_LOGE(TAG, "WS error!"); s_connected = false; break;
        }
        {  /* parse and handle */
            cJSON *root = cJSON_ParseWithLength(msg, msg_len);
            if (root) {
                cJSON *type = cJSON_GetObjectItem(root, "type");
                cJSON *txt  = cJSON_GetObjectItem(root, "text");

                /* ── scan_wifi: server requests WiFi scan for network location ── */
                if (cJSON_IsString(type) && strcmp(type->valuestring, "scan_wifi") == 0) {
                    ESP_LOGI(TAG, "scan_wifi: starting scan");
                    wifi_ap_info_t aps[WIFI_SCAN_MAX_APS];
                    int count = wifi_scan_aps(aps);
                    ESP_LOGI(TAG, "scan_wifi: found %d APs", count);
                    static char wifi_json[800];
                    static char resp[1024];
                    int wj_len = wifi_scan_build_json(aps, count, wifi_json, sizeof(wifi_json));
                    int n = snprintf(resp, sizeof(resp),
                             "{\"type\":\"scan_result\",\"wifiinfo\":%.*s}",
                             wj_len > 0 ? wj_len : 2,
                             wj_len > 0 ? wifi_json : "[]");
                    ESP_LOGI(TAG, "scan_wifi: sending result (%d bytes, %d APs)", n, count);
                    ws_client_send_json(resp);
                }
                /* ── get_memory: 服务端工具 /tools.history 拉取完整对话记忆 ── */
                else if (cJSON_IsString(type) && strcmp(type->valuestring, "get_memory") == 0) {
                    char *mem = NULL; size_t mem_len = 0;
                    memory_store_get(&mem, &mem_len);
                    cJSON *resp = cJSON_CreateObject();
                    cJSON_AddStringToObject(resp, "type", "memory_data");
                    cJSON *req_id = cJSON_GetObjectItem(root, "req_id");
                    if (cJSON_IsString(req_id))
                        cJSON_AddStringToObject(resp, "req_id", req_id->valuestring);
                    cJSON_AddNumberToObject(resp, "size", (double)mem_len);
                    cJSON_AddStringToObject(resp, "content", mem ? mem : "");
                    char *json = cJSON_PrintUnformatted(resp);
                    cJSON_Delete(resp);
                    if (json) {
                        ESP_LOGI(TAG, "memory_data: %u B (req=%s)", (unsigned)mem_len,
                                 cJSON_IsString(req_id) ? req_id->valuestring : "?");
                        ws_client_send_json(json);
                        cJSON_free(json);
                    }
                    if (mem) free(mem);
                }
                /* ── memory_update: 服务端压缩后下发覆盖 ── */
                else if (cJSON_IsString(type) && strcmp(type->valuestring, "memory_update") == 0) {
                    cJSON *content = cJSON_GetObjectItem(root, "content");
                    if (cJSON_IsString(content))
                        memory_store_overwrite(content->valuestring);
                }
                else if (cJSON_IsString(type) && cJSON_IsString(txt)) {
                    if (strcmp(type->valuestring, "chat_done") == 0 ||
                        strcmp(type->valuestring, "chat_reply") == 0) {
                        s_chat_seq++;   /* 会话模式等回复的信号 */
                        /* 服务端回显的用户原文 (后续阶段用于设备端记忆) */
                        cJSON *llm_user = cJSON_GetObjectItem(root, "user_text");
                        if (cJSON_IsString(llm_user)) {
                            snprintf(s_last_user_text, sizeof(s_last_user_text), "%s",
                                     llm_user->valuestring);
                        }
                        /* strip |pXXX and [tags] for log, |pXXX only for TTS */
                        char clean[512], tts_text[512], instruction[384] = {0};
                        const char *src = txt->valuestring;
                        int ci = 0, ti = 0;
                        while (*src && ci < sizeof(clean)-1) {
                            if (src[0]=='|' && src[1]=='p' && src[2]>='0' && src[2]<='9') {
                                src+=2; while (*src>='0'&&*src<='9') src++;
                                continue;
                            }
                            /* Extract [inst: ...] for TTS instruction */
                            if (strncmp(src, "[inst:", 6) == 0) {
                                src += 6;
                                const char *end = strchr(src, ']');
                                if (end) {
                                    int ilen = end - src;
                                    if (ilen >= (int)sizeof(instruction)) ilen = sizeof(instruction)-1;
                                    memcpy(instruction, src, ilen);
                                    instruction[ilen] = '\0';
                                    src = end + 1;
                                    continue;
                                }
                            }
                            if (*src == '[') {
                                const char *end = strchr(src, ']');
                                if (end) { src = end + 1; continue; }
                            }
                            /* Skip /tools.xxx or /tools.xxx(params) */
                            if (strncmp(src, "/tools.", 7) == 0) {
                                src += 7;
                                while (*src && (*src != ' ' && *src != '(' && *src != ',')) src++;
                                if (*src == '(') { const char *e = strchr(src, ')'); if (e) src = e+1; }
                                while (*src == ' ') src++;
                                continue;
                            }
                            clean[ci++] = *src++;
                        }
                        clean[ci] = '\0';
                        src = txt->valuestring;
                        while (*src && ti < sizeof(tts_text)-1) {
                            if (src[0]=='|' && src[1]=='p' && src[2]>='0' && src[2]<='9') {
                                src+=2; while (*src>='0'&&*src<='9') src++;
                                continue;
                            }
                            if (strncmp(src, "[inst:", 6) == 0) {
                                src += 6;
                                const char *end = strchr(src, ']');
                                if (end) { src = end + 1; continue; }
                            }
                            /* Skip /tools.xxx or /tools.xxx(params) */
                            if (strncmp(src, "/tools.", 7) == 0) {
                                src += 7;
                                while (*src && (*src != ' ' && *src != '(' && *src != ',')) src++;
                                if (*src == '(') { const char *e = strchr(src, ')'); if (e) src = e+1; }
                                while (*src == ' ') src++;
                                continue;
                            }
                            tts_text[ti++] = *src++;
                        }
                        tts_text[ti] = '\0';

                        /* 解析动画 */
                        cJSON *llm_anim = cJSON_GetObjectItem(root, "animation");
                        pet_anim_t anim = PET_ANIM_COUNT;
                        if (cJSON_IsString(llm_anim))
                            anim = parse_anim_name(llm_anim->valuestring);

                        ESP_LOGI(TAG, "萝莉丝: %s", clean);

                        /* 动画先播 — 与流式 TTS 下载/播放并行, 不再等语音 */
                        if (anim < PET_ANIM_COUNT &&
                            strcmp(llm_anim->valuestring, "none") != 0)
                            pet_avatar_play(anim);

                        /* 设备端记忆: 追加本轮对话 (flash 写在 TTS 开播前, 不卡音频) */
                        if (s_last_user_text[0] && clean[0])
                            memory_store_append(s_last_user_text, clean);

                        /* 空文本 = 静默模式 (只做动作不说话) */
                        if (tts_text[0]) {
                            /* 新回复打断旧播放; 失败则兜底立即显示气泡
                               [inst:] 自然语言语音控制已禁用 — 不同输入大幅
                               改变音色不稳定, 标签仍剥除不进 TTS */
                            bool tts_ok = tts_speak(tts_text);
                            if (!tts_ok)
                                tts_ok = tts_client_interrupt_speak(tts_text);
                            if (!tts_ok) {
                                ESP_LOGE(TAG, "tts FAILED");
                                chat_bubble_show(txt->valuestring, 8000);
                            } else {
                                /* 立即打字机 (不再等 TTS 开播), 停留时长按字数估算 */
                                chat_bubble_show(txt->valuestring,
                                                 2000 + strlen(txt->valuestring) * 220);
                            }
                        } else {
                            chat_bubble_show(txt->valuestring, 8000);
                        }

                        /* Pet engine: 新协议 mood_delta 增量直喂; 旧服务端(mood 绝对值)降级求差 */
                        cJSON *llm_mood_d = cJSON_GetObjectItem(root, "mood_delta");
                        cJSON *llm_mood   = cJSON_GetObjectItem(root, "mood");
                        cJSON *llm_exp    = cJSON_GetObjectItem(root, "exp");
                        int8_t exp_d = cJSON_IsNumber(llm_exp)
                                        ? (int8_t)llm_exp->valueint : 0;
                        if (cJSON_IsNumber(llm_mood_d)) {
                            pet_process_chat((int8_t)llm_mood_d->valueint, exp_d);
                        } else if (cJSON_IsNumber(llm_mood)) {
                            pet_state_t st = pet_engine_get_state();
                            pet_process_chat((int8_t)llm_mood->valueint - (int8_t)st.mood,
                                             exp_d);
                        }
                    }
                }
                cJSON_Delete(root);
            }
        }
        }  /* end WEBSOCKET_EVENT_DATA block */
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WS 错误"); s_connected = false; break;
    default: break;
    }
}

/* cJSON 全走 PSRAM — memory_data 的 100KB 字符串内嵌 RAM 放不下 */
static void *cjson_psram_malloc(size_t sz) { return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM); }

esp_err_t ws_client_connect(const char *token) {
    cJSON_InitHooks(&(cJSON_Hooks){ .malloc_fn = cjson_psram_malloc, .free_fn = free });
    char uri[512];
    snprintf(uri, sizeof(uri), "ws://%s:%d/ws/device?token=%s", SERVER_HOST, SERVER_PORT, token);
    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .ping_interval_sec = 30,          /* 30s ping 检测死连接 (服务器重启后自动重连) */
        .pingpong_timeout_sec = 10,
        .reconnect_timeout_ms = 10000,
        .network_timeout_ms = 10000,
        .task_stack = 16384,              /* cJSON + FatFS 记忆追加调用链深, 12KB 会溢出 */
    };
    s_client = esp_websocket_client_init(&cfg);
    if (!s_client) return ESP_FAIL;
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    return esp_websocket_client_start(s_client);
}

bool ws_client_is_connected(void) { return s_connected; }
esp_err_t ws_client_send_text(const char *text) {
    if (!s_client || !s_connected) return ESP_FAIL;
    char json[512]; snprintf(json, sizeof(json), "{\"type\":\"chat\",\"text\":\"%s\"}", text);
    return esp_websocket_client_send_text(s_client, json, strlen(json), 0);
}
esp_err_t ws_client_send_json(const char *json) {
    if (!s_client || !s_connected) return ESP_FAIL;
    return esp_websocket_client_send_text(s_client, json, strlen(json), 0);
}

esp_err_t ws_client_send_chat(const char *text) {
    /* cJSON 构建: 正确处理引号/换行 + 附带记忆元数据 (缓存值, 零 FatFS 访问 —
     * 本函数会在 sess 任务 12KB 栈上执行, FatFS+WL 调用链曾栈溢出) */
    if (!s_client || !s_connected) return ESP_FAIL;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "chat");
    cJSON_AddStringToObject(root, "text", text);
    const char *summary = memory_store_cached_summary();
    if (summary)
        cJSON_AddStringToObject(root, "mem_summary", summary);
    cJSON_AddNumberToObject(root, "mem_size", (double)memory_store_cached_size());
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_FAIL;
    esp_err_t err = esp_websocket_client_send_text(s_client, json, strlen(json), 0);
    cJSON_free(json);
    return err;
}
