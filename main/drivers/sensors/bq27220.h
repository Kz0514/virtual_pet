/**
 * @file bq27220.h
 * @brief BQ27220 电池电量计 (I2C 0x55, SBS协议)
 *
 * 读取: 电压(mV)/电流(mA)/电量(%)/剩余容量(mAh)/温度(°C)/健康度/循环次数
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t voltage_mv;
    int16_t  current_ma;      /* +充电, -放电 */
    uint16_t soc_pct;         /* 0-100% */
    uint16_t remain_mah;
    uint16_t full_mah;
    float    temp_c;
    uint16_t health_pct;      /* SOH 0-100% */
    uint16_t cycle_count;
} bq27220_data_t;

esp_err_t bq27220_init(void);
esp_err_t bq27220_read(bq27220_data_t *out);
esp_err_t bq27220_read_soc(uint16_t *soc_pct);

/** 扫描 SBS 寄存器 (0x00-0x3F), 打印所有有效读数到日志 */
void bq27220_debug_scan(void);

#ifdef __cplusplus
}
#endif
