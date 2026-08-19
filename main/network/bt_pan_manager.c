/**
 * @file bt_pan_manager.c
 * @brief 蓝牙 PAN 管理器 (Bluetooth PAN Profile - PANU角色)
 *
 * WiFi不可用时通过手机蓝牙共享网络上网. ESP32 BNEP→IP, WebSocket透明重连.
 * 网络优先级: WiFi > BLE PAN. 带宽约200-700kbps, 足够Opus语音流(~16kbps).
 */
#include "esp_err.h"
#include "esp_log.h"
static const char *TAG = "bt_pan";
esp_err_t bt_pan_manager_init(void) { ESP_LOGI(TAG, "蓝牙PAN管理器 (桩)"); return ESP_OK; }
