/**
 * @file qmc6309.h
 * @brief QMC6309 3轴地磁传感器 (I2C 0x2C, 经 MPU6500 AUX 旁路访问)
 *
 * 硬件特殊性: QMC6309 不直连 I2C0, 而是挂在 MPU6500 的 AUX I2C 上.
 * MPU6500 初始化时使能 INT_PIN_CFG.BYPASS_EN 后, QMC6309 可通过主 I2C0 直接访问.
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float x;       /*!< X轴磁场 (μT) */
    float y;       /*!< Y轴磁场 (μT) */
    float z;       /*!< Z轴磁场 (μT) */
    float heading; /*!< 方位角 (度, 0-360, 北=0) */
} qmc6309_data_t;

/** 初始化 QMC6309 (连续测量模式, 100Hz, ±2G, OSR512) */
esp_err_t qmc6309_init(void);

/** 读取地磁数据 */
esp_err_t qmc6309_read(qmc6309_data_t *data);

#ifdef __cplusplus
}
#endif
