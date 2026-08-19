/**
 * @file diary_screen.c
 * @brief 日记浏览: 按月列表 → 详情(Markdown渲染 + 涂鸦图片)
 */
#include "esp_err.h"
#include "esp_log.h"
static const char *TAG = "diary_screen";
esp_err_t diary_screen_init(void) { ESP_LOGI(TAG, "日记界面 (桩)"); return ESP_OK; }
