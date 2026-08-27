/**
 * @file ws_client.c
 * @brief WebSocket 客户端 — 服务器实时通信 (仅文本, 不做音频流)
 */
#include "ws_client.h"
#include "server_config.h"
#include "tts_client.h"
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
static bool s_paused = false;            /* 息屏期暂停 (WiFi 已停, 重连必败) */
/* 恒为聚合模式: 服务器 LLM 流式期间增量合成, WS 二进制帧直推
 * (audio_start/binary/audio_end)。
 * s_ws_audio_seen = 本轮收到过 audio_start — 音频已由 WS 流推送。
 * 置位归传输层 (audio_start), 消费归协议层 (message_handler 在 chat_done
 * 经 ws_audio_stream_take_seen() 读出即清零);
 * 未收到 = 服务器 TTS 失败 → 兜底整段 POST */
static bool s_ws_audio_seen = false;

/* UTF-8 清洗: 坏字节就地替换为 '?' (长度不变, 返回坏字节数, 输出仍合法
 * UTF-8)。坏点源于 512B snprintf 行尾截断 — 半个字符进记忆/上送会致服务端
 * 拒连, '?' 占位使其余字节解析不受影响。 */
static size_t utf8_sanitize_inplace(char *s, size_t len)
{
    size_t i = 0, bad = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        size_t need;
        if (c < 0x80) {
            i++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF)
            need = 2; /* 2 字节 */
        else if (c >= 0xE0 && c <= 0xEF)
            need = 3; /* 3 字节 */
        else if (c >= 0xF0 && c <= 0xF4)
            need = 4; /* 4 字节 */
        else {
            s[i] = '?';
            bad++;
            i++;
            continue;
        }                               /* 孤立 continuation/非法起始 */
        bool is_bad = (i + need > len); /* 字符被截断 */
        for (size_t k = 1; !is_bad && k < need; k++) {
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) is_bad = true; /* continuation 非法 */
        }
        if (is_bad) {
            s[i] = '?';
            bad++;
            i++;
            continue;
        } /* 坏起始字节 — 占位, 后续重新解析 */
        i += need;
    }
    return bad;
}

/* 分片重组缓冲 (PSRAM) — 大消息 (memory_update 等) 按片到达 */
static char *s_rx_buf = NULL;
static int s_rx_len = 0, s_rx_total = 0;

/* 本轮 WS 音频流消费接口 — 读出即清零 (message_handler 在 chat_done 用):
 * true = audio_start 收到过 (音频已 WS 直推, 跳过整段 TTS POST)。
 * 清零同时承担原"一轮结束"语义 — 防跨轮误判兜底 POST */
bool ws_audio_stream_take_seen(void)
{
    bool v = s_ws_audio_seen;
    s_ws_audio_seen = false;
    return v;
}

/* 协议帧回调 (app/message_handler 注册): 返回 true = 已认领, 跳过默认链。
 * 未注册时全部帧走内部默认链 — 与重构前逐位一致。 */
static ws_frame_handler_t s_frame_handler = NULL;

void ws_client_set_frame_handler(ws_frame_handler_t cb) { s_frame_handler = cb; }

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
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
        /* WS 音频流模式: 断连 = 不会有更多音频也不会有 audio_end —
         * 立即置 rb_done 让播放排空收尾, 免等超时 */
        if (tts_client_is_ws_active())
            tts_client_ws_end();
        if (s_rx_buf) {
            free(s_rx_buf);
            s_rx_buf = NULL;
        } /* 丢弃半截消息 */
        s_rx_len = s_rx_total = 0;
        break;
    case WEBSOCKET_EVENT_DATA: {
        /* ── 二进制帧 = WS 音频流 PCM ──
         * 逐片直接入环, 绝不分片重组: PCM 是字节流, 环按序消费, 分片
         * 边界无意义; 播放期 PSRAM 已被双槽环+动画帧占满, 重组按消息
         * malloc 大缓冲必失败。零分配零拷贝, 组件任务最轻。 */
        if (evt->op_code == 2 && evt->data_len > 0) {
            tts_client_ws_feed((const uint8_t *)evt->data_ptr,
                               (uint32_t)evt->data_len);
            break;
        }
        /* 分片消息重组: payload_len > data_len 表示本条只是其中一片
         * (仅文本帧走此路径 — 二进制帧已在上方直接喂环) */
        const char *msg;
        int msg_len;
        if (evt->payload_len > evt->data_len) {
            if (!s_rx_buf) {
                s_rx_buf = heap_caps_malloc(evt->payload_len + 1, MALLOC_CAP_SPIRAM);
                if (!s_rx_buf) {
                    ESP_LOGE(TAG, "重组缓冲分配失败");
                    break;
                }
                s_rx_len = 0;
                s_rx_total = evt->payload_len;
            }
            memcpy(s_rx_buf + evt->payload_offset, evt->data_ptr, evt->data_len);
            s_rx_len += evt->data_len;
            if (s_rx_len < s_rx_total) break; /* 等剩余分片 */
            s_rx_buf[s_rx_len] = '\0';
            msg = s_rx_buf;
            msg_len = s_rx_len;
            s_rx_buf = NULL;
            s_rx_len = s_rx_total = 0;
        } else {
            msg = (const char *)evt->data_ptr;
            msg_len = evt->data_len;
        }
        /* Only log JSON messages (ignore binary/control frames) */
        if (msg_len > 0 && msg[0] == '{')
            ESP_LOGI(TAG, "<<< %.*s", msg_len, msg);
        if (strstr(msg, "\"error\"") ||
            strstr(msg, "未授权")) {
            ESP_LOGE(TAG, "WS error!");
            s_connected = false;
            break;
        }
        { /* parse and handle */
            cJSON *root = cJSON_ParseWithLength(msg, msg_len);
            if (root) {
                cJSON *type = cJSON_GetObjectItem(root, "type");
                cJSON *txt = cJSON_GetObjectItem(root, "text");

                /* 协议帧回调: 已注册且认领 (返回 true) → 跳过默认链;
                 * 未注册/未认领 → 走下方默认链, 与重构前逐位一致 */
                if (cJSON_IsString(type) && s_frame_handler &&
                    s_frame_handler(type->valuestring, root)) {
                    cJSON_Delete(root);
                    break;
                }
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
                /* ── audio_start / audio_end: WS 音频流边界 — LLM 流式期间
                    增量合成的 PCM 经二进制帧直推, 本帧只做起链/收尾 ── */
                else if (cJSON_IsString(type) &&
                         strcmp(type->valuestring, "audio_start") == 0) {
                    if (tts_client_ws_start()) {
                        s_ws_audio_seen = true;
                        ESP_LOGI(TAG, "WS 音频流起链");
                    } else {
                        ESP_LOGW(TAG, "audio_start: WS 起链失败");
                    }
                } else if (cJSON_IsString(type) &&
                           strcmp(type->valuestring, "audio_end") == 0) {
                    tts_client_ws_end();
                    ESP_LOGI(TAG, "WS 音频流结束");
                }
                cJSON_Delete(root);
            }
        }
    } /* end WEBSOCKET_EVENT_DATA block */
    break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WS 错误");
        s_connected = false;
        break;
    default:
        break;
    }
}

/* cJSON 全走 PSRAM — memory_data 的 100KB 字符串内嵌 RAM 放不下 */
static void *cjson_psram_malloc(size_t sz) { return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM); }

esp_err_t ws_client_connect(const char *token)
{
    /* 幂等 + 可重建: 组件内置重连只覆盖"已连接后断开", 不覆盖创建任务
     * 失败 — 主循环 30s 周期调用本函数重建客户端; 已连则直接跳过。 */
    if (s_paused) return ESP_OK; /* 息屏期不重建 (WiFi 已停) */
    if (s_connected) return ESP_OK;
    if (s_client) {
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }
    s_connected = false;
    cJSON_InitHooks(&(cJSON_Hooks){.malloc_fn = cjson_psram_malloc, .free_fn = free});
    char uri[512];
    snprintf(uri, sizeof(uri), "ws://%s:%d/ws/device?token=%s", SERVER_HOST, SERVER_PORT, token);
    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .ping_interval_sec = 30, /* 30s ping 检测死连接 (服务器重启后自动重连) */
        .pingpong_timeout_sec = 10,
        .reconnect_timeout_ms = 10000,
        .network_timeout_ms = 10000,
        /* 调用链深需大栈 — 组件栈由 PSRAM 分配 (vendored 组件改用
         * xTaskCreatePinnedToCoreWithCaps); 16384 恰好对齐
         * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL, 普通 malloc 必走内部 RAM */
        .task_stack = 16384,
    };
    s_client = esp_websocket_client_init(&cfg);
    if (!s_client) return ESP_FAIL;
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    return esp_websocket_client_start(s_client);
}

bool ws_client_is_connected(void) { return s_connected; }

/* 息屏暂停/亮屏恢复 — 与 main.c 的息屏/亮屏块配对:
 * stop 停掉组件及其内置重连计时器 (WiFi 停后重连必失败), start 重新
 * 发起连接; 对象保留, 无需重建。 */
void ws_client_pause(void)
{
    s_paused = true;
    if (s_client) esp_websocket_client_stop(s_client);
    s_connected = false;
    ESP_LOGI(TAG, "WS 暂停 (息屏)");
}

void ws_client_resume(void)
{
    s_paused = false;
    if (s_client) {
        esp_err_t ret = esp_websocket_client_start(s_client);
        if (ret != ESP_OK)
            ESP_LOGE(TAG, "WS 恢复失败: %s — 主循环 30s 周期会重建",
                     esp_err_to_name(ret));
    }
    ESP_LOGI(TAG, "WS 恢复 (亮屏)");
}
esp_err_t ws_client_send_text(const char *text)
{
    if (!s_client || !s_connected) return ESP_FAIL;
    char json[512];
    snprintf(json, sizeof(json), "{\"type\":\"chat\",\"text\":\"%s\"}", text);
    return esp_websocket_client_send_text(s_client, json, strlen(json), pdMS_TO_TICKS(100));
}
esp_err_t ws_client_send_json(const char *json)
{
    if (!s_client || !s_connected) return ESP_FAIL;
    return esp_websocket_client_send_text(s_client, json, strlen(json), pdMS_TO_TICKS(100));
}

esp_err_t ws_client_send_chat(const char *text)
{
    /* cJSON 构建: 正确处理引号/换行 + 附带记忆元数据 (缓存值, 零 FatFS 访问 —
     * 本函数在 sess 任务 12KB 栈上执行, 不引入文件调用链) */
    if (!s_client || !s_connected) return ESP_FAIL;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "chat");
    /* ASR 文本同样清洗 — 任何坏字节进 WS 帧都会被服务端拒连:
     * 坏字节替换 '?', 全坏发空串 */
    {
        size_t tl = strlen(text);
        char *ttmp = heap_caps_malloc(tl + 1, MALLOC_CAP_SPIRAM);
        if (ttmp) {
            memcpy(ttmp, text, tl + 1);
            size_t bad = utf8_sanitize_inplace(ttmp, tl);
            cJSON_AddStringToObject(root, "text", (bad < tl) ? ttmp : "");
            free(ttmp);
        } else {
            cJSON_AddStringToObject(root, "text", text); /* PSRAM 不足 — 原样发 (极端场景) */
        }
    }
    /* 兜底: 记忆摘要缓存再验一次 UTF-8 — 坏字节上送 = 服务端拒连;
     * 坏字节替换 '?', 全坏不带。 */
    const char *summary = memory_store_cached_summary();
    if (summary) {
        size_t sl = strlen(summary);
        char *tmp = heap_caps_malloc(sl + 1, MALLOC_CAP_SPIRAM);
        if (tmp) {
            memcpy(tmp, summary, sl + 1);
            size_t bad = utf8_sanitize_inplace(tmp, sl);
            if (bad < sl) cJSON_AddStringToObject(root, "mem_summary", tmp);
            free(tmp);
        } else {
            cJSON_AddStringToObject(root, "mem_summary", summary); /* PSRAM 不足 — 原样发 */
        }
    }
    cJSON_AddNumberToObject(root, "mem_size", (double)memory_store_cached_size());
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_FAIL;
    esp_err_t err = esp_websocket_client_send_text(s_client, json, strlen(json), pdMS_TO_TICKS(100));
    cJSON_free(json);
    return err;
}
