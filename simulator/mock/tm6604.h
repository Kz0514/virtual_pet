/** @file mock/tm6604.h — Mock TM6604 线性马达驱动 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 模拟器无马达 — no-op */
static inline void tm6604_vibrate(uint8_t duty_pct, uint16_t duration_ms) {
    (void)duty_pct;
    (void)duration_ms;
}

#ifdef __cplusplus
}
#endif
