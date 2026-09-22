/**
 * @file hw_i2c.h
 * @brief B 段 I2C 总线体检 (由 I2C 总线诊断工具移植而来)
 *
 * ⚠️ 顺序纪律 (踩坑记录):
 *   gpio_config(GPIO_MODE_INPUT_OUTPUT_OD) 会把外设输出路由从 pad 上摘掉,
 *   之后 i2c_master_* 一律超时, 只有 i2c_new_master_bus 能恢复。
 *   ⇒ 位翻转段必须排在所有"外设访问"之前或之后, 且中间要重建总线。
 *   本模块的调用顺序固定为:
 *     hw_i2c_levels()  → hw_i2c_bitbang()  → hw_i2c_bus_open()
 *     → hw_i2c_bus_scan() → [C 段器件] → [F 段音频] → hw_i2c_stress()
 *     → hw_i2c_bridge()  → hw_i2c_bus_close() → hw_i2c_sda_probe()
 */
#ifndef HW_I2C_H
#define HW_I2C_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"

/* B1 空闲电平 (纯 GPIO, 建总线前) */
void hw_i2c_levels(void);
/* B2 上升时间 + 位翻转扫描/归因 (纯 GPIO) */
void hw_i2c_bitbang(void);
/* 建/拆总线 (重建外设路由) */
esp_err_t hw_i2c_bus_open(void);
void hw_i2c_bus_close(void);
i2c_master_bus_handle_t hw_i2c_bus(void);
/* B3 外设路径全地址扫描 + 期望比对 */
void hw_i2c_bus_scan(void);
/* B4 满速连打压力 + 间隔扫描 + 0x7C↔0x55 交替 */
void hw_i2c_stress(void);
/* B5 MCLK 开关压力 (LEDC 假 MCLK — 关键实验) */
void hw_i2c_mclk_stress(void);
/* B6 双向拖动 (会摘掉 pad 所有权 → 必须最后跑) */
void hw_i2c_bridge(void);
/* B7 SDA 直流体检: ADC 实测电压 → 外部上拉/下拉等效阻值 (纯引脚操作, 排在 B6 之后) */
void hw_i2c_sda_probe(void);
/* B7 是否判出异常 (B8 量线窗口的开关) */
bool hw_i2c_sda_suspect(void);
/* B8 量线窗口: SDA 钉低 20 秒给人拿表量 (只在 B7 判异常时开) */
void hw_i2c_sda_hold(void);

/* 假 MCLK 开关 (供 F 段音频前后复用) */
esp_err_t hw_mclk_start(void);
void hw_mclk_stop(void);

const char *hw_dev_name(uint8_t addr);
/* 引脚当前电平: true = 两线都放开 (未被任何器件钳住) */
bool hw_i2c_lines_free(void);

#endif /* HW_I2C_H */
