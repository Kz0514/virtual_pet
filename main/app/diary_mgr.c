/**
 * @file diary_mgr.c
 * @brief 本地日记管理: LittleFS存储, Markdown格式, 每日互动>阈值时触发生成
 */
#include "esp_err.h"
#include "esp_log.h"
static const char *TAG = "diary_mgr";
esp_err_t diary_mgr_init(void) { ESP_LOGI(TAG, "日记管理 (桩)"); return ESP_OK; }
