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
esp_lcd_panel_handle_t    st7789_get_panel(void);
esp_lcd_panel_io_handle_t st7789_get_panel_io(void);

#ifdef __cplusplus
}
#endif
