/**
 * @file time_manager.c
 * @brief NTP 时间同步 + FreeRTOS 软件定时器管理
 */
#include "esp_err.h"
#include "esp_log.h"
static const char *TAG = "time_manager";
esp_err_t time_manager_init(void) { ESP_LOGI(TAG, "NTP 时间同步 + FreeRTOS 软件定时器管理 (桩)"); return ESP_OK; }
