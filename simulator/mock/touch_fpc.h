/** @file mock/touch_fpc.h — Mock FPC 触摸驱动
 *  模拟器用 SDL 鼠标代替触摸, 所有触摸查询返回"未按下" */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline void touch_fpc_scan(void) { /* no-op */ }
static inline bool touch_is_left_pressed(void) { return false; }
static inline uint32_t touch_left_hold_ms(void) { return 0; }
static inline float touch_top_position(void) { return 0.5f; }
static inline float touch_right_position(void) { return 0.5f; }
static inline bool touch_is_right_pressed(void) { return false; }
static inline bool touch_is_petting_head(void) { return false; }
static inline bool touch_is_top_middle_pressed(void) { return false; }

static inline void touch_get_raw(uint32_t *raw_out) {
    for (int i = 0; i < 12; i++) raw_out[i] = 0;
}
static inline void touch_get_baseline(uint32_t *bl_out) {
    for (int i = 0; i < 12; i++) bl_out[i] = 0;
}
static inline void touch_get_filtered(int *filtered_out) {
    for (int i = 0; i < 12; i++) filtered_out[i] = 0;
}

#ifdef __cplusplus
}
#endif
