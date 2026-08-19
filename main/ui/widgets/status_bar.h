/** @file status_bar.h @brief 顶部状态栏 — WiFi + 电池 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void status_bar_init(void);
void status_bar_set_wifi(bool connected, int8_t rssi);
void status_bar_set_battery(uint8_t pct, uint16_t voltage_mv);

#ifdef __cplusplus
}
#endif