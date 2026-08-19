/** @file mock/esp_task_wdt.h — Mock Task Watchdog Timer */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t timeout_ms;
    uint32_t idle_core_mask;
    bool trigger_panic;
} esp_task_wdt_config_t;

static inline void esp_task_wdt_deinit(void) { /* no-op */ }

static inline void esp_task_wdt_init(const esp_task_wdt_config_t *cfg) {
    (void)cfg; /* no-op */
}

#ifdef __cplusplus
}
#endif
