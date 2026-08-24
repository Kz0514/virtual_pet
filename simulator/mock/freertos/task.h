/** @file mock/freertos/task.h — Minimal FreeRTOS task mock */
#pragma once
#include "FreeRTOS.h"

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 真实睡眠: anim_load 等常驻任务由 xTaskCreate 真线程执行, vTaskDelay
 * 必须让出 CPU (no-op 会 100% 自旋)。settings_screen 重启前 flush 会
 * 实眠 200ms — 与实机一致, 可接受 */
static inline void vTaskDelay(TickType_t ticks) {
    usleep((useconds_t)(ticks) * 1000u);
}
static inline void vTaskDelete(TaskHandle_t task) { (void)task; /* no-op */ }

/* font_loader.c 使用此宏在 CPU1 创建字体加载任务
 * 模拟器同步执行任务函数 (PC 上解析很快, 无需真线程) */
#define xTaskCreatePinnedToCore(task_func, name, stack, arg, prio, handle, core) \
    do { *(handle) = (void*)1; (void)(core); (task_func)(arg); } while(0)

/* ── xTaskCreate: pet_avatar.c 常驻任务 (anim_load 死循环 / probe 一次性)
 * 模拟器起真线程并发执行 — 复刻固件双核语义, 帧加载与 LVGL 主循环并发;
 * 互斥由 FreeRTOS.h 的 portMUX 真锁保证 (指针发布/释放都在锁内) */
typedef struct {
    void (*fn)(void *);
    void *arg;
} sim_task_s;

static inline void *sim_task_entry(void *p)
{
    sim_task_s *s = (sim_task_s *)p;
    pthread_detach(pthread_self());
    s->fn(s->arg);
    free(s);
    return NULL;
}

static inline int sim_task_start(void (*fn)(void *), void *arg, TaskHandle_t *handle)
{
    /* 与 FreeRTOS 一致: handle 可为 NULL (调用方不需要句柄) */
    sim_task_s *s = (sim_task_s *)malloc(sizeof(*s));
    if (!s) { if (handle) *handle = NULL; return pdFAIL; }
    s->fn = fn;
    s->arg = arg;
    pthread_t t;
    if (pthread_create(&t, NULL, sim_task_entry, s) != 0) {
        free(s);
        if (handle) *handle = NULL;
        return pdFAIL;
    }
    if (handle) *handle = (void *)1;
    return pdPASS;
}

#define xTaskCreate(task_func, name, stack, arg, prio, handle) \
    sim_task_start((task_func), (arg), (handle))

#ifdef __cplusplus
}
#endif
