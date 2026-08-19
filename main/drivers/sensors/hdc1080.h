/**
 * @file hdc1080.h
 * @brief HDC1080 温湿度传感器驱动接口 (I2C 0x40)
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** HDC1080 测量结果 */
typedef struct {
    float temperature;  /* 摄氏度 °C */
    float humidity;     /* 相对湿度 %RH */
} hdc1080_data_t;

/** 初始化 HDC1080: 验证设备ID, 配置14位分辨率 */
esp_err_t hdc1080_init(void);

/** 触发一次温湿度测量 (阻塞 ~20ms), 返回结果 */
esp_err_t hdc1080_read(hdc1080_data_t *out);

#ifdef __cplusplus
}
#endif
