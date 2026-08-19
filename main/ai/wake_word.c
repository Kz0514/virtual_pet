/**
 * @file wake_word.c
 * @brief ESP-SR 本地唤醒词检测 (MultiNet/WakeNet)
 *
 * 唤醒词: "萝莉丝". 模型存储在assets分区(约2-5MB), 启动时加载到PSRAM.
 * 检测延迟<100ms. 检测到唤醒词后发布 EVENT_WAKE_WORD 事件.
 */
#include "esp_err.h"
#include "esp_log.h"
static const char *TAG = "wake_word";
esp_err_t wake_word_init(void) { ESP_LOGI(TAG, "唤醒词检测 (桩)"); return ESP_OK; }
