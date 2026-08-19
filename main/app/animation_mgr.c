/**
 * @file animation_mgr.c
 * @brief 动画管理器: 独立帧(RGB565) + JSON清单, 双缓冲零拷贝播放
 *
 * assets/animations/<name>/frame_000.rgb565 + manifest.json
 * PSRAM帧池30帧(≈3.5MB), 按需预加载, LVGL lv_img_set_src()零拷贝切换
 */
#include "esp_err.h"
#include "esp_log.h"
static const char *TAG = "animation_mgr";
esp_err_t animation_mgr_init(void) { ESP_LOGI(TAG, "动画管理器 (桩)"); return ESP_OK; }
