/**
 * @file hw_periph.h
 * @brief D 段 面板 / E 段 触摸 / G 段 马达 —— 全部无人工参与
 *
 * 诚实边界 (别把"没测到"当"测过了"):
 *   - 面板 SPI **没有 MISO** → 读不了 RDDID/MADCTL/COLMOD。
 *     D 段只能验: SPI 事务全部成功 + 115200B 整帧 DMA 完成 + RST/DC 脚没被短路。
 *     面板是否真收下像素 → 靠最后那块看板人工旁证 (不是判据)。
 *   - 触摸有人在场会污染基线 → 判定只看"有没有读数/噪声会不会自触发", 不要求人划。
 */
#ifndef HW_PERIPH_H
#define HW_PERIPH_H

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

/* D 段: SPI2 + ST7789 写通路 + 背光 LEDC (置屏幕为开, 供末尾看板) */
void hw_periph_lcd(void);
/* E 段: 12 通道触摸 初始化 + 基线/噪声 */
void hw_periph_touch(void);
/* G 段: 马达 EN 脚 + PWM 通道 (全程 EN 拉低 — 马达不会动) */
void hw_periph_haptic(void);

/* 屏幕看板用 (D 段跑过之后才有值) */
esp_err_t hw_lcd_get(esp_lcd_panel_handle_t *panel, esp_lcd_panel_io_handle_t *io);
/* 整屏 240x240 RGB565 同步推完 (等 DMA 完成回调, 超时 2s) */
esp_err_t hw_lcd_blit(const void *fb);

#endif /* HW_PERIPH_H */
