/**
 * @file weather_overlay.c
 * @brief 实时天气效果叠加层(lv_layer_top): 雨滴粒子/雪花飘落/闪电闪白/雾霾半透明
 *
 * 服务端每15分钟拉取腾讯天气API, 天气变化时推送0x21更新. 设备端300ms渐变切换效果.
 */
#include "esp_err.h"
#include "esp_log.h"
static const char *TAG = "weather_overlay";
esp_err_t weather_overlay_init(void) { ESP_LOGI(TAG, "天气叠加层 (桩)"); return ESP_OK; }
