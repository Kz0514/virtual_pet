/**
 * @file gpio_utils.h
 * @brief GPIO 控制工具接口
 */
#pragma once

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 配置引脚为推挽输出, 设置初始电平 */
esp_err_t gpio_output_init(gpio_num_t pin, uint32_t initial_level);

/** 设置输出电平 (0=低, 非0=高) */
esp_err_t gpio_set(gpio_num_t pin, uint32_t level);

/** 读取引脚电平 */
int gpio_get(gpio_num_t pin);

#ifdef __cplusplus
}
#endif
