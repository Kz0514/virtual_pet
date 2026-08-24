/** @file mock/tm6604.h — Mock TM6604 线性马达驱动 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 模拟器无马达 — no-op */
static inline void tm6604_vibrate(uint8_t duty_pct, uint16_t duration_ms) {
    (void)duty_pct;
    (void)duration_ms;
}

/* 导航轻震 (设置页/日记页用 10-bit 原始占空比) */
static inline void tm6604_vibrate_raw(uint16_t duty_raw, uint16_t duration_ms) {
    (void)duty_raw;
    (void)duration_ms;
}

static inline bool tm6604_is_vibrating(void) {
    return false;
}

#ifdef __cplusplus
}
#endif
