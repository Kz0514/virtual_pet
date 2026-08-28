/**
 * @file weather_client.c
 * @brief 天气客户端实现 — ④-7a 自 main.c 迁出 (行为逐位一致)
 *
 * 数据流: GET /api/v1/weather/ip_location → adcode →
 *         GET /api/v1/weather/current?city=<adcode> → 日志输出。
 * 静默失败设计: 任一环节失败即返回, 不重试 (开机单次)。
 */
#include "weather_client.h"
#include "http_util.h"
#include "api_client.h"
#include "server_config.h"
#include "esp_log.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "weather";

void weather_fetch_once(void)
{
    const char *token = api_client_get_token();
    if (!token || !token[0]) return;
    char url[384];
    snprintf(url, sizeof(url), "http://%s:%d/api/v1/weather/ip_location?token=%s",
             SERVER_HOST, SERVER_PORT, token);
    char *body = http_get(url, 1024);
    if (!body) return;
    cJSON *loc = cJSON_Parse(body);
    free(body);
    if (!loc)
        return;
    cJSON *ad = cJSON_GetObjectItem(loc, "adcode");
    char adcode_str[16] = "110101";
    if (cJSON_IsString(ad))
        strncpy(adcode_str, ad->valuestring, sizeof(adcode_str) - 1);
    /* : city 缺失会解引用 NULL — 服务端契约恒有该字段, 保持原行为 */
    ESP_LOGI(TAG, "📍 %s (adcode=%s)",
             cJSON_GetObjectItem(loc, "city")->valuestring, adcode_str);
    cJSON_Delete(loc);

    snprintf(url, sizeof(url), "http://%s:%d/api/v1/weather/current?city=%s&token=%s",
             SERVER_HOST, SERVER_PORT, adcode_str, token);
    body = http_get(url, 1024);
    if (!body) return;
    cJSON *w = cJSON_Parse(body);
    free(body);
    if (w) {
        ESP_LOGI(TAG, "🌤 %s %s°C 湿度:%s%%",
                 cJSON_GetObjectItem(w, "weather")->valuestring,
                 cJSON_GetObjectItem(w, "temperature")->valuestring,
                 cJSON_GetObjectItem(w, "humidity")->valuestring);
        cJSON_Delete(w);
    }
}