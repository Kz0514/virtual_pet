/**
 * @file ota_client.c
 * @brief OTA firmware update
 */
#include "ota_client.h"
#include "server_config.h"
#include "notify_overlay.h"
#include "esp_log.h"
#include "esp_err.h"
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

static uint8_t *s_fw_buf = NULL;
static int s_fw_size = 0;
static int s_fw_offset = 0;

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
    if (err != ESP_OK || status != 200) return false;

    cJSON *root = cJSON_Parse(s_buf);
    if (!root) return false;
    if (!cJSON_IsTrue(cJSON_GetObjectItem(root, "update_available"))) {
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

    /* 2. Download — use Content-Length from server, not DB */
    snprintf(url, sizeof(url), "http://%s:%d/api/v1/ota/download/%s",
             SERVER_HOST, SERVER_PORT, latest_ver);
    free(latest_ver);

    /* Allocate buffer (DB size is upper bound) */
    s_fw_buf = malloc(fw_size);
    if (!s_fw_buf) { ESP_LOGE(TAG, "malloc fail"); return false; }

    esp_http_client_config_t dl_cfg = {
        .url = url, .timeout_ms = 120000, .keep_alive_enable = false,
    };
    esp_http_client_handle_t dl_cli = esp_http_client_init(&dl_cfg);

    err = esp_http_client_open(dl_cli, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open fail"); esp_http_client_cleanup(dl_cli); free(s_fw_buf); return false;
    }

    int content_len = esp_http_client_fetch_headers(dl_cli);
    int dl_status = esp_http_client_get_status_code(dl_cli);
    if (dl_status != 200 || content_len <= 0) {
        ESP_LOGE(TAG, "bad response: status=%d", dl_status);
        esp_http_client_close(dl_cli); esp_http_client_cleanup(dl_cli); free(s_fw_buf); return false;
    }

    /* Read until stream ends — SHA256 ensures integrity */
    s_fw_offset = 0;
    while (s_fw_offset < fw_size) {
        int rd = esp_http_client_read(dl_cli, (char *)s_fw_buf + s_fw_offset,
                                       fw_size - s_fw_offset);
        if (rd <= 0) break;
        s_fw_offset += rd;
    }
    esp_http_client_close(dl_cli);
    esp_http_client_cleanup(dl_cli);
    ESP_LOGI(TAG, "Download: %d bytes", s_fw_offset);

    if (s_fw_offset == 0) { free(s_fw_buf); return false; }
    s_fw_size = s_fw_offset;
    notify_show(NOTIFY_INFO, "Download OK, verifying...", 0);

    /* 2b. SHA256 verification */
    if (expected_sha[0]) {
        uint8_t hash[32];
        char hash_hex[65];
        mbedtls_sha256(s_fw_buf, s_fw_offset, hash, 0);
        for (int i = 0; i < 32; i++) sprintf(hash_hex + i * 2, "%02x", hash[i]);
        hash_hex[64] = '\0';

        if (strncmp(hash_hex, expected_sha, 64) != 0) {
            ESP_LOGE(TAG, "SHA256 mismatch!");
            notify_show(NOTIFY_ERROR, "OTA failed: SHA256 mismatch", 5000);
            free(s_fw_buf); return false;
        }
        ESP_LOGI(TAG, "SHA256 OK: %.16s...", hash_hex);
    }

    /* 3. OTA write */
    notify_show(NOTIFY_INFO, "Installing update...", 0);
    const esp_partition_t *ota_part = esp_ota_get_next_update_partition(NULL);
    ESP_LOGI(TAG, "OTA begin %s (%d bytes)", ota_part->label, s_fw_offset);

    esp_ota_handle_t handle;
    err = esp_ota_begin(ota_part, s_fw_offset, &handle);
    if (err != ESP_OK) { ESP_LOGE(TAG, "begin fail: %s", esp_err_to_name(err)); free(s_fw_buf); return false; }

    err = esp_ota_write(handle, s_fw_buf, s_fw_offset);
    free(s_fw_buf); s_fw_buf = NULL;
    if (err != ESP_OK) { ESP_LOGE(TAG, "write fail: %s", esp_err_to_name(err)); return false; }

    err = esp_ota_end(handle);
    ESP_LOGI(TAG, "end: %s", esp_err_to_name(err));
    if (err != ESP_OK) return false;

    err = esp_ota_set_boot_partition(ota_part);
    ESP_LOGI(TAG, "set_boot: %s", esp_err_to_name(err));
    if (err != ESP_OK) return false;

    ESP_LOGI(TAG, "OTA OK, rebooting...");
    notify_show(NOTIFY_INFO, "Update complete! Rebooting...", 3000);
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return true;
}

void ota_check_task(void *pvParameter)
{
    vTaskDelay(pdMS_TO_TICKS(3000));

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t desc;
    char version[32] = "1.0.0";
    if (running && esp_ota_get_partition_description(running, &desc) == ESP_OK)
        snprintf(version, sizeof(version), "%s", desc.version);
    ESP_LOGI(TAG, "OTA task: v%s", version);

    ota_client_check_and_update("", version);
    ESP_LOGI(TAG, "OTA task exit");
    vTaskDelete(NULL);
}
