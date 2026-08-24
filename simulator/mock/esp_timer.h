/** @file mock/esp_timer.h — Mock ESP timer (单调时钟, 微秒) */
#pragma once
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 与实机 esp_timer_get_time() 同语义: 上电以来的微秒数 (单调) */
static inline int64_t esp_timer_get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

#ifdef __cplusplus
}
#endif
