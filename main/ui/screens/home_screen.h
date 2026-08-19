/**
 * @file home_screen.h
 * @brief 主屏幕 — LVGL 界面, 传感器数据/WiFi状态
 *
 * 线程安全: home_screen_set_data() 可从任何线程调用(仅写全局变量).
 *           LVGL 更新由内部 lv_timer 在 LVGL 线程执行.
 */
#pragma once
#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void home_screen_create(void);
void home_screen_set_data(float temp, float humidity, float lux,
                           uint16_t battery_mv, uint16_t battery_pct,
                           float bat_temp_c, int16_t current_ma,
                           uint16_t remain_mah, uint16_t full_mah,
                           bool wifi_connected, const char *wifi_ip);
void home_screen_set_chat(const char *text);
void home_screen_append_chat(const char *text, bool first);

#ifdef __cplusplus
}
#endif
