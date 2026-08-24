/**
 * @file st7789.h
 * @brief ST7789 240x240 SPI 显示屏硬件驱动接口
 *
 * LVGL 集成由 esp_lvgl_port 组件管理.
 * 本模块仅暴露硬件初始化和背光控制.
 */
#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t st7789_init(void);
esp_err_t st7789_backlight_set(uint8_t percent);
esp_lcd_panel_handle_t st7789_get_panel(void);
esp_lcd_panel_io_handle_t st7789_get_panel_io(void);

/** : 面板芯片睡眠 — 息屏时 ST7789 仍在 DISPON 以 ~119Hz 内部全帧
 * 扫描 (normal mode 耗电 ~3-5mA), 背光关了白烧。sleep=true:
 * DISPOFF → SLPIN (振荡器停, ~20µA 级); sleep=false: SLPOUT →
 * 等 120ms (datasheet 稳定期, 短了花屏) → DISPON。由
 * power_manager_screen_off/on 调用 (LVGL 上下文, SPI 传输安全)。 */
esp_err_t st7789_panel_sleep(bool sleep);

#ifdef __cplusplus
}
#endif
