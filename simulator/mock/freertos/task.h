/** @file mock/freertos/task.h — Minimal FreeRTOS task mock */
#pragma once
#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline void vTaskDelay(TickType_t ticks) { (void)ticks; /* no-op */ }
static inline void vTaskDelete(TaskHandle_t task) { (void)task; /* no-op */ }

/* font_loader.c 使用此宏在 CPU1 创建字体加载任务
 * 模拟器同步执行任务函数 (PC 上解析很快, 无需真线程) */
#define xTaskCreatePinnedToCore(task_func, name, stack, arg, prio, handle, core) \
    do { *(handle) = (void*)1; (void)(core); (task_func)(arg); } while(0)

#ifdef __cplusplus
}
#endif
