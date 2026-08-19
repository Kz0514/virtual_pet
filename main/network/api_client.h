/**
 * @file api_client.h
 * @brief HTTP REST 客户端 — 设备注册 + JWT Token 管理
 */
#pragma once
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t api_client_init(void);
const char *api_client_get_token(void);
bool api_client_is_authenticated(void);

#ifdef __cplusplus
}
#endif
