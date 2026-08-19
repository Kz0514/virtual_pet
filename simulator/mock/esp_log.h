/** @file mock/esp_log.h — Mock ESP-IDF logging → printf */
#pragma once
#include <stdio.h>
#include <stdint.h>
#include <string.h>   /* ESP-IDF's esp_log.h transitively includes this */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_LOG_NONE = 0,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE
} esp_log_level_t;

#define ESP_LOGE(tag, fmt, ...)  printf("[E] %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...)  printf("[W] %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...)  printf("[I] %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...)  printf("[D] %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...)  printf("[V] %s: " fmt "\n", tag, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif
