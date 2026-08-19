/**
 * @file net_selector.c
 * @brief 网络选择策略: WiFi优先 → BLE PAN备用 → 离线模式(本地缓存)
 *
 * 切换时自动重连WebSocket, 对上层透明.
 */
#include "esp_err.h"
#include "esp_log.h"
static const char *TAG = "net_selector";
esp_err_t net_selector_init(void) { ESP_LOGI(TAG, "网络选择器 (桩)"); return ESP_OK; }
