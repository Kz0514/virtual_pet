/** @file mock/esp_lvgl_port.h — Mock esp_lvgl_port (LVGL 显示接入层)
 *
 * 模拟器单线程 (lv_timer_handler 主循环), 无需互斥锁 — no-op 即可。
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline bool lvgl_port_lock(uint32_t timeout_ms) {
    (void)timeout_ms;
    return true;
}

static inline void lvgl_port_unlock(void) {}

#ifdef __cplusplus
}
#endif
