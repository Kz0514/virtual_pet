/** @file tm6604.h @brief TM6604 线性马达驱动 (GPIO38 EN + GPIO39 PWM) */
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 GPIO + LEDC PWM */
esp_err_t tm6604_init(void);

/** 震动: duty_pct=0~100, duration_ms 持续时间. 调用后立即返回, 定时器自动停止 */
void tm6604_vibrate(uint8_t duty_pct, uint16_t duration_ms);

/** 震动(10-bit 原始占空比 0~1023): 整数百分比粒度不够时用
 *  (本马达 ~50% 起振, 如导航轻震用 537 ≈ 52.5%) */
void tm6604_vibrate_raw(uint16_t duty_raw, uint16_t duration_ms);

/** 马达是否正在震动 (供摇动/敲击检测抑制 — 震动会经外壳传给 DMP) */
bool tm6604_is_vibrating(void);

#ifdef __cplusplus
}
#endif
