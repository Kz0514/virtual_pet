/** @file mock/esp_err.h — Mock ESP-IDF error codes */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef int esp_err_t;

#define ESP_OK               0
#define ESP_FAIL             -1
#define ESP_ERR_NO_MEM       -2
#define ESP_ERR_NOT_SUPPORTED -3

/* pet_avatar.c 的 mmap 失败分支引用 (模拟器无分区, 该分支为死代码但需声明) */
static inline const char *esp_err_to_name(esp_err_t code) {
    switch (code) {
    case ESP_OK:               return "ESP_OK";
    case ESP_ERR_NO_MEM:       return "ESP_ERR_NO_MEM";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    default:                   return "ESP_FAIL";
    }
}

#ifdef __cplusplus
}
#endif
