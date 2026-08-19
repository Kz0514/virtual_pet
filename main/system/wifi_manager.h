/**
 * @file wifi_manager.h
 * @brief WiFi 连接管理 + SoftAP 配网门户
 *
 * 自动流程: NVS有凭据→直连 / 无凭据→开启SoftAP配网
 * Captive Portal: DNS劫持所有域名→HTTP配网页→保存密码→重连
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_DISCONNECTED,
    WIFI_CONNECTING,
    WIFI_CONNECTED,
    WIFI_SOFTAP_MODE,     /* 配网模式 */
} wifi_state_t;

esp_err_t wifi_manager_init(void);
wifi_state_t wifi_get_state(void);
bool wifi_is_connected(void);
const char *wifi_get_ip(void);

#ifdef __cplusplus
}
#endif
