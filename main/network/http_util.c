/**
 * @file http_util.c
 * @brief HTTP GET 统一封装实现 — ④-7a 自 app/diary_sync 抽出 (行为逐位一致)
 */
#include "http_util.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include <string.h>

static const char *TAG = "http";

#define HTTP_TIMEOUT_MS 10000

/* 响应收集上下文 — event handler 经 user_data 访问 */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} get_ctx_t;

static esp_err_t get_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    get_ctx_t *ctx = evt->user_data;
    size_t room = ctx->cap - ctx->len - 1;
    size_t n = evt->data_len < room ? evt->data_len : room;
    memcpy(ctx->buf + ctx->len, evt->data, n);
    ctx->len += n;
    return ESP_OK;
}

char *http_get(const char *url, size_t cap)
{
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "缓冲分配失败 (%uB)", (unsigned)cap);
        return NULL;
    }
    get_ctx_t ctx = {buf, cap, 0};

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = get_handler,
        .user_data = &ctx,
        .timeout_ms = HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(buf);
        return NULL;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "GET 失败: err=%d status=%d url=%.80s", err, status, url);
        free(buf);
        return NULL;
    }
    buf[ctx.len] = '\0';
    return buf;
}