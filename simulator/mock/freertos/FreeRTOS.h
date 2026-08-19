/** @file mock/freertos/FreeRTOS.h — Minimal FreeRTOS mock */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void * TaskHandle_t;
typedef uint32_t TickType_t;
typedef int32_t BaseType_t;

typedef enum {
    eDeleted = 0,
    eReady = 1,
    eRunning = 2,
    eBlocked = 3,
    eSuspended = 4,
} eTaskState;

static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void*)1; }
static inline eTaskState eTaskGetState(TaskHandle_t t) { (void)t; return eDeleted; }
static inline TickType_t xTaskGetTickCount(void) { return 0; }

#define pdMS_TO_TICKS(ms)       ((ms) / portTICK_PERIOD_MS)
#define portTICK_PERIOD_MS      1
#define pdPASS                  1
#define pdFAIL                  0

#ifdef __cplusplus
}
#endif
