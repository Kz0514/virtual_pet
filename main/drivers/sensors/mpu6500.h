/**
 * @file    mpu6500.h
 * @brief   MPU6500 6轴加速度/陀螺仪驱动接口
 *
 * I2C地址: 0x68 (AD0=0)
 * 测量范围: ±4g (加速度), ±250dps (陀螺仪)
 * 采样率:   ~200Hz (SMPLRT_DIV=3, 内部1kHz / 4)
 * AUX旁路:  使能 → QMC6309 (0x2C) 可通过主I2C总线直接访问
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** MPU6500 传感器数据 */
typedef struct {
    /* 加速度 (g, ±2g量程, 16384 LSB/g) */
    float accel_x;
    float accel_y;
    float accel_z;

    /* 陀螺仪 (dps, ±250dps量程, 131 LSB/dps) */
    float gyro_x;
    float gyro_y;
    float gyro_z;

    /* 芯片温度 (°C) */
    float temperature;
} mpu6500_data_t;

/**
 * @brief 初始化 MPU6500
 * - 硬件复位 + 唤醒
 * - 配置 ±2g / ±250dps 量程
 * - 使能 AUX I2C 旁路 (用于 QMC6309)
 * - 使能运动中断 (加速度 > 阈值 → GPIO40)
 */
esp_err_t mpu6500_init(void);

/**
 * @brief 读取加速度 + 陀螺仪 + 温度
 * @param[out] data 传感器数据结构体
 * @return ESP_OK 成功, 其他值表示 I2C 读取失败
 */
esp_err_t mpu6500_read(mpu6500_data_t *data);

esp_err_t mpu6500_read_qmc6309(float *mx, float *my, float *mz, float *heading);

/** Expose I2C device handle for DMP driver reuse */
i2c_master_dev_handle_t mpu6500_get_i2c_dev(void);

/** 打印所有配置寄存器 + 一组传感器读数, 用于调试 */
void mpu6500_debug_scan(void);

#ifdef __cplusplus
}
#endif
