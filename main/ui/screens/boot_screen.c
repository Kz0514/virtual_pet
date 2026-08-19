/**
 * @file boot_screen.c
 * @brief 启动引导/配网界面: Logo动画 + SoftAP配网指引 + WiFi连接进度
 */
#include "esp_err.h"
#include "esp_log.h"
static const char *TAG = "boot_screen";
esp_err_t boot_screen_init(void) { ESP_LOGI(TAG, "启动/配网界面 (桩)"); return ESP_OK; }
