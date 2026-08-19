/** @file mock/esp_err.h — Mock ESP-IDF error codes */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef int esp_err_t;

#define ESP_OK          0
#define ESP_FAIL        -1
#define ESP_ERR_NO_MEM  -2

#ifdef __cplusplus
}
#endif
