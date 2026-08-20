/**
 * @file ota_client.c
 * @brief OTA firmware update
 */
#include "ota_client.h"
#include "server_config.h"
#include "settings_screen.h"   /* 1.0.227: OTA 提示走设置页内嵌提示框 */
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "ota";
static char s_buf[2048];
static int  s_buf_len = 0;

static bool s_check_requested = false;   /* 设置页"检查更新"请求标志 */
static bool s_manual = false;            /* 手动检查: 附加 overlay 结果反馈 */

/* 流式下载: 边读边校验边写 OTA 分区 — 不再整包 malloc 固件 (1.4MB 真机上
 * 曾静默失败且无下载请求到达服务端, 整包缓冲+整包写入是首要嫌疑; 流式 +
 * 逐步日志保证下次失败点可见) */
static esp_err_t ota_stream_download(esp_http_client_handle_t cli, int content_len,
                                     const char *expected_sha)
{
    const esp_partition_t *ota_part = esp_ota_get_next_update_partition(NULL);
    if (!ota_part) { ESP_LOGE(TAG, "no OTA partition"); return ESP_FAIL; }

    esp_ota_handle_t handle;
    esp_err_t err = esp_ota_begin(ota_part, OTA_SIZE_UNKNOWN, &handle);
    if (err != ESP_OK) { ESP_LOGE(TAG, "ota_begin fail: %s", esp_err_to_name(err)); return err; }

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);

    /* 8KB 分块缓冲, 优先 PSRAM */
    uint8_t *buf = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(8192);
    if (!buf) { ESP_LOGE(TAG, "chunk buf fail"); esp_ota_abort(handle); return ESP_FAIL; }

    int total = 0, last_pct = -1;
    while (1) {
        int rd = esp_http_client_read(cli, (char *)buf, 8192);
        if (rd < 0) { ESP_LOGE(TAG, "read err %d @ %d bytes", rd, total); break; }
        if (rd == 0) break;                              /* stream EOF */
        mbedtls_sha256_update(&ctx, buf, rd);
        err = esp_ota_write(handle, buf, rd);
        if (err != ESP_OK) { ESP_LOGE(TAG, "ota_write fail: %s", esp_err_to_name(err)); break; }
        total += rd;
        if (content_len > 0) {                           /* 进度通知, 每 10% */
            int pct = total * 100 / content_len;
            if (pct - last_pct >= 10) {
                last_pct = pct;
                char msg[24];
                snprintf(msg, sizeof(msg), "升级中 %d%%", pct);
                settings_screen_notify(NOTIFY_INFO, msg, 3000);
                ESP_LOGI(TAG, "download %d%% (%d/%d)", pct, total, content_len);
            }
        }
    }
    free(buf);

    if (total == 0) { ESP_LOGE(TAG, "download empty"); esp_ota_abort(handle); return ESP_FAIL; }
    if (content_len > 0 && total != content_len) {
        ESP_LOGE(TAG, "size mismatch: got %d want %d", total, content_len);
        esp_ota_abort(handle); return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Download OK: %d bytes", total);

    /* SHA256 校验 (流式累积) */
    if (expected_sha[0]) {
        uint8_t hash[32];
        char hash_hex[65];
        mbedtls_sha256_finish(&ctx, hash);
        for (int i = 0; i < 32; i++) sprintf(hash_hex + i * 2, "%02x", hash[i]);
        hash_hex[64] = '\0';
        if (strncmp(hash_hex, expected_sha, 64) != 0) {
            ESP_LOGE(TAG, "SHA256 mismatch: %.16s... vs %.16s...", hash_hex, expected_sha);
            esp_ota_abort(handle);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "SHA256 OK: %.16s...", hash_hex);
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) { ESP_LOGE(TAG, "ota_end fail: %s", esp_err_to_name(err)); return err; }
    err = esp_ota_set_boot_partition(ota_part);
    if (err != ESP_OK) { ESP_LOGE(TAG, "set_boot fail: %s", esp_err_to_name(err)); return err; }
    ESP_LOGI(TAG, "OTA complete (%d bytes) — rebooting", total);
    return ESP_OK;
}

static esp_err_t http_event_cb(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA && s_buf_len + evt->data_len < sizeof(s_buf) - 1) {
        memcpy(s_buf + s_buf_len, evt->data, evt->data_len);
        s_buf_len += evt->data_len;
        s_buf[s_buf_len] = '\0';
    }
    return ESP_OK;
}

bool ota_client_check_and_update(const char *token, const char *current_version)
{
    char url[512];

    /* 1. Check */
    snprintf(url, sizeof(url), "http://%s:%d/api/v1/ota/check?current_version=%s",
             SERVER_HOST, SERVER_PORT, current_version);
    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_GET,
        .event_handler = http_event_cb, .timeout_ms = 10000,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    s_buf_len = 0;
    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    ESP_LOGI(TAG, "Check: err=%d status=%d", err, status);
    if (err != ESP_OK || status != 200) {
        if (s_manual) settings_screen_notify(NOTIFY_ERROR, "检查失败, 请稍后重试", 3000);
        return false;
    }

    cJSON *root = cJSON_Parse(s_buf);
    if (!root) {
        if (s_manual) settings_screen_notify(NOTIFY_ERROR, "检查失败", 3000);
        return false;
    }
    if (!cJSON_IsTrue(cJSON_GetObjectItem(root, "update_available"))) {
        if (s_manual) settings_screen_notify(NOTIFY_INFO, "已是最新版本", 3000);
        ESP_LOGI(TAG, "Up to date"); cJSON_Delete(root); return false;
    }
    char *latest_ver = strdup(cJSON_GetObjectItem(root, "latest_version")->valuestring);
    int fw_size = cJSON_GetObjectItem(root, "file_size")->valueint;
    cJSON *sha = cJSON_GetObjectItem(root, "sha256");
    char expected_sha[65] = "";
    if (cJSON_IsString(sha)) strncpy(expected_sha, sha->valuestring, 64);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "New: %s -> %s (%d bytes) sha=%.16s...",
             current_version, latest_ver, fw_size, expected_sha);

    /* 2. Download — 流式: 边读边校验边写, 不整包 malloc */
    snprintf(url, sizeof(url), "http://%s:%d/api/v1/ota/download/%s",
             SERVER_HOST, SERVER_PORT, latest_ver);
    free(latest_ver);

    esp_http_client_config_t dl_cfg = {
        .url = url, .timeout_ms = 120000, .keep_alive_enable = false,
    };
    esp_http_client_handle_t dl_cli = esp_http_client_init(&dl_cfg);
    if (!dl_cli) { ESP_LOGE(TAG, "download init fail"); return false; }

    err = esp_http_client_open(dl_cli, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "download open fail: %s", esp_err_to_name(err));
        esp_http_client_cleanup(dl_cli); return false;
    }

    int content_len = esp_http_client_fetch_headers(dl_cli);
    int dl_status = esp_http_client_get_status_code(dl_cli);
    if (dl_status != 200 || content_len <= 0) {
        ESP_LOGE(TAG, "bad download response: status=%d len=%d", dl_status, content_len);
        esp_http_client_close(dl_cli); esp_http_client_cleanup(dl_cli); return false;
    }
    ESP_LOGI(TAG, "Download start: %d bytes", content_len);
    settings_screen_notify(NOTIFY_INFO, "Downloading...", 3000);

    err = ota_stream_download(dl_cli, content_len, expected_sha);
    esp_http_client_close(dl_cli);
    esp_http_client_cleanup(dl_cli);
    if (err != ESP_OK) {
        settings_screen_notify(NOTIFY_ERROR, "OTA 失败, 请重试", 5000);
        return false;
    }

    ESP_LOGI(TAG, "OTA OK, rebooting...");
    settings_screen_notify(NOTIFY_INFO, "Update complete! Rebooting...", 3000);
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return true;
}

void ota_check_task(void *pvParameter)
{
    /* 注册成功后由主循环同步调用 (ota_client_check_sync),
     * 此任务仅保留接口兼容 — 见 ota_client_check_sync() */
    ota_client_check_sync();
    vTaskDelete(NULL);
}

void ota_client_check_sync(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t desc;
    char version[32] = "0.0.0";   /* 失败占位: 保证服务端 lat>cur, 不因读版本失败而错过升级 */
    if (running && esp_ota_get_partition_description(running, &desc) == ESP_OK)
        snprintf(version, sizeof(version), "%s", desc.version);
    ESP_LOGI(TAG, "OTA check: v%s", version);
    ota_client_check_and_update("", version);
    ESP_LOGI(TAG, "OTA check done");
}

void ota_client_request_check(void)
{
    s_check_requested = true;
}

void ota_client_check_poll(void)
{
    if (!s_check_requested) return;
    s_check_requested = false;
    settings_screen_notify(NOTIFY_INFO, "检查更新中…", 0);
    s_manual = true;                 /* 反馈"已是最新/检查失败" (下载各阶段本就通知) */
    ota_client_check_sync();
    s_manual = false;
}
