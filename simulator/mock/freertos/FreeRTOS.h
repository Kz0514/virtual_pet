/** @file mock/freertos/FreeRTOS.h — Minimal FreeRTOS mock */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void * TaskHandle_t;
typedef uint32_t TickType_t;
typedef int32_t BaseType_t;

/* ── portMUX (pet_avatar.c 帧加载互斥): 真互斥锁.
 * anim_load 任务是真线程 (见 task.h), 锁内转移/释放的语义必须与
 * 固件一致 (帧指针发布 + unload 释放都在锁内), 否则模拟器端会出
 * 固件上不会出现的竞态 */
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
static inline void portENTER_CRITICAL(portMUX_TYPE *m) { pthread_mutex_lock(m); }
static inline void portEXIT_CRITICAL(portMUX_TYPE *m) { pthread_mutex_unlock(m); }

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
