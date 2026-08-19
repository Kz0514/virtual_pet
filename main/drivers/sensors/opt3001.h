/**
 * @file opt3001.h
 * @brief OPT3001-Q1 环境光传感器驱动接口 (I2C 0x44)
 *
 * 量程: 0.01 ~ 83,000 lux, 自动量程切换
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 OPT3001: 连续测量模式, 自动量程, 转换时间800ms */
esp_err_t opt3001_init(void);

/** 读取环境光照度 (lux), 首次读取需等 ~800ms 转换时间 */
esp_err_t opt3001_read_lux(float *lux);

#ifdef __cplusplus
}
#endif
