/**
 * @file asset_client.c
 * @brief SPIFFS 资源 OTA — 检查→对比 SHA256→下载→写入
 */
#include "asset_client.h"
#include "server_config.h"
#include "notify_overlay.h"
#include "api_client.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_spiffs.h"
#include "mbedtls/sha256.h"
#include "cJSON.h"
#include <string.h>
#include <sys/stat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "asset";

static char s_buf[4096];
static int  s_buf_len;

static esp_err_t http_cb(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA && s_buf_len + evt->data_len < sizeof(s_buf) - 1) {
        memcpy(s_buf + s_buf_len, evt->data, evt->data_len);
        s_buf_len += evt->data_len;
        s_buf[s_buf_len] = '\0';
    }
    return ESP_OK;
}

/* Compute SHA256 of a SPIFFS file. Returns hex string or NULL. */
static bool file_sha256(const char *path, char out_hex[65]) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    uint8_t buf[1024];
    while (1) {
        size_t n = fread(buf, 1, sizeof(buf), fp);
        if (n == 0) break;
        mbedtls_sha256_update(&ctx, buf, n);
    }
    fclose(fp);
    uint8_t hash[32];
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
    for (int i = 0; i < 32; i++) sprintf(out_hex + i * 2, "%02x", hash[i]);
    out_hex[64] = '\0';
    return true;
}

/* Download a single file from server, write to SPIFFS */
static bool download_asset(const char *filename, int expected_size) {
    char url[384], path[128];
    snprintf(url, sizeof(url), "http://%s:%d/api/v1/assets/download/%s",
             SERVER_HOST, SERVER_PORT, filename);
    snprintf(path, sizeof(path), "/spiffs/%s", filename);

    esp_http_client_config_t cfg = {
        .url = url, .timeout_ms = 60000, .keep_alive_enable = false,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) { esp_http_client_cleanup(cli); return false; }

    int clen = esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    if (status != 200 || clen <= 0) {
        esp_http_client_close(cli); esp_http_client_cleanup(cli); return false;
    }

    /* Allocate buffer for this file */
    uint8_t *buf = malloc(clen);
    if (!buf) { esp_http_client_close(cli); esp_http_client_cleanup(cli); return false; }

    int off = 0;
    while (off < clen) {
        int rd = esp_http_client_read(cli, (char *)buf + off, clen - off);
        if (rd <= 0) break;
        off += rd;
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);

    if (off != clen) { free(buf); return false; }

    /* Write to SPIFFS */
    FILE *fp = fopen(path, "wb");
    if (!fp) { free(buf); return false; }
    size_t written = fwrite(buf, 1, clen, fp);
    fclose(fp);
    free(buf);

    return written == (size_t)clen;
}

void asset_update_task(void *pvParameter) {
    vTaskDelay(pdMS_TO_TICKS(2000));  /* wait for API client */

    const char *token = api_client_get_token();
    if (!token || !token[0]) {
        ESP_LOGW(TAG, "No token, skip asset check");
        vTaskDelete(NULL); return;
    }

    /* 1. Get asset list from server */
    char url[384];
    snprintf(url, sizeof(url), "http://%s:%d/api/v1/assets/list?token=%s",
             SERVER_HOST, SERVER_PORT, token);

    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_GET,
        .event_handler = http_cb, .timeout_ms = 10000,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    s_buf_len = 0;
    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "List failed: err=%d status=%d", err, status);
        vTaskDelete(NULL); return;
    }

    cJSON *root = cJSON_Parse(s_buf);
    if (!root) { vTaskDelete(NULL); return; }
    cJSON *files = cJSON_GetObjectItem(root, "files");
    if (!cJSON_IsArray(files)) { cJSON_Delete(root); vTaskDelete(NULL); return; }

    int updated = 0, checked = 0;
    int count = cJSON_GetArraySize(files);
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(files, i);
        const char *name = cJSON_GetObjectItem(item, "name")->valuestring;
        const char *srv_sha = cJSON_GetObjectItem(item, "sha256")->valuestring;
        int srv_size = cJSON_GetObjectItem(item, "size")->valueint;
        if (!name || !srv_sha) continue;

        char path[128];
        snprintf(path, sizeof(path), "/spiffs/%s", name);
        checked++;

        /* Check local file */
        char local_sha[65] = "";
        bool match = false;
        if (file_sha256(path, local_sha)) {
            match = (strncmp(local_sha, srv_sha, 64) == 0);
        }

        if (match) {
            ESP_LOGI(TAG, "  ✓ %s", name);
        } else {
            ESP_LOGI(TAG, "  ↓ %s (%d bytes) %s", name, srv_size,
                     local_sha[0] ? "updated" : "new");
            if (download_asset(name, srv_size)) {
                /* Verify after download */
                if (file_sha256(path, local_sha) &&
                    strncmp(local_sha, srv_sha, 64) == 0) {
                    ESP_LOGI(TAG, "    ✓ verified");
                    updated++;
                } else {
                    ESP_LOGW(TAG, "    ✗ verify failed!");
                }
            } else {
                ESP_LOGW(TAG, "    ✗ download failed");
            }
        }
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Assets: %d checked, %d updated", checked, updated);
    if (updated > 0) notify_show(NOTIFY_INFO, "Assets updated!", 5000);
    vTaskDelete(NULL);
}
