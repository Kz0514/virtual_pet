/**
 * @file api_client.c
 * @brief HTTP REST 客户端 — 设备注册 + JWT 令牌管理
 *
 * 流程: 读 NVS token → 无效则 POST /api/v1/devices/register → 保存 token
 */
#include "api_client.h"
#include "server_config.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "api";
static char s_token[512] = {0};

#define NVS_NAMESPACE   "device"
#define NVS_KEY_TOKEN   "jwt"

/* ── 获取 MAC 地址字符串 ── */
static void get_mac_str(char *out, size_t len)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ── HTTP 事件处理 ── */
static char s_recv_buf[1024];
static int  s_recv_len = 0;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (s_recv_len + evt->data_len < sizeof(s_recv_buf) - 1) {
            memcpy(s_recv_buf + s_recv_len, evt->data, evt->data_len);
            s_recv_len += evt->data_len;
            s_recv_buf[s_recv_len] = '\0';
        }
        break;
    default: break;
    }
    return ESP_OK;
}

/* ── 初始化 ── */
esp_err_t api_client_init(void)
{
    /* 1. 强制重注册 (跳过NVS旧token, 避免签名不匹配) */
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_key(nvs, NVS_KEY_TOKEN); /* 清除旧token */
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    /* 2. 无 token — 注册设备 */
    char mac[18];
    get_mac_str(mac, sizeof(mac));
    ESP_LOGI(TAG, "注册设备 MAC=%s ...", mac);

    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/api/v1/devices/register", SERVER_HOST, SERVER_PORT);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);

    /* 构建 JSON body */
    char body[256];
    snprintf(body, sizeof(body),
             "{\"mac_address\":\"%s\",\"device_name\":\"萝莉丝\",\"firmware_version\":\"1.0.23\"}", mac);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    s_recv_len = 0;
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "注册失败: err=%d status=%d resp=%s", err, status, s_recv_buf);
        return ESP_FAIL;
    }

    /* 3. 解析 JSON 获取 token */
    cJSON *root = cJSON_Parse(s_recv_buf);
    if (!root) { ESP_LOGE(TAG, "JSON 解析失败"); return ESP_FAIL; }

    cJSON *tok = cJSON_GetObjectItem(root, "token");
    if (cJSON_IsString(tok)) {
        strncpy(s_token, tok->valuestring, sizeof(s_token) - 1);
        ESP_LOGI(TAG, "设备注册成功! token: %.20s...", s_token);
    }
    cJSON_Delete(root);

    if (!s_token[0]) { ESP_LOGE(TAG, "token 为空"); return ESP_FAIL; }

    /* 4. 保存到 NVS */
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, NVS_KEY_TOKEN, s_token);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    return ESP_OK;
}

const char *api_client_get_token(void) { return s_token; }
bool api_client_is_authenticated(void) { return s_token[0] != '\0'; }

/* ── 设备时区查询 (IP 定位 → 服务端换算偏移秒) ── */
int32_t api_get_timezone_offset(void)
{
    if (!api_client_is_authenticated()) return -1;

    char url[640]; /* host + 512B token + 路径, 防 format-truncation */
    snprintf(url, sizeof(url), "http://%s:%d/api/v1/location/timezone?token=%s",
             SERVER_HOST, SERVER_PORT, s_token);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = http_event_handler,
        .timeout_ms = 8000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    s_recv_len = 0;
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "时区查询失败: err=%d status=%d", err, status);
        return -1;
    }

    cJSON *root = cJSON_Parse(s_recv_buf);
    if (!root) { ESP_LOGW(TAG, "时区响应 JSON 解析失败"); return -1; }
    cJSON *off = cJSON_GetObjectItem(root, "tz_offset_sec");
    int32_t res = (cJSON_IsNumber(off)) ? (int32_t)off->valuedouble : -1;
    cJSON_Delete(root);
    return res;
}
