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

/**
 * 查询设备时区偏移 (秒) — 服务端按 IP 定位缓存换算, 失败/未认证返回 -1。
 * 同步阻塞调用 (超时 8s), 每日一次即可, 勿高频调用。
 */
int32_t api_get_timezone_offset(void);

#ifdef __cplusplus
}
#endif
