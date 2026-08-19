/**
 * @file sensor_fusion.c
 * @brief 多传感器融合事件检测: 环境噪音/光照/加速度/温湿度→高层语义事件
 *
 * 例: 光照<10lux + 时间>22:00 → "主人已关灯", 加速度>2g → "摇动检测"
 */
#include "esp_err.h"
#include "esp_log.h"
static const char *TAG = "sensor_fusion";
esp_err_t sensor_fusion_init(void) { ESP_LOGI(TAG, "传感器融合 (桩)"); return ESP_OK; }
