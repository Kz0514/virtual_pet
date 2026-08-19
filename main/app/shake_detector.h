/** @file shake_detector.h @brief 摇动检测 — DMP 原始加速度回调 + 高通峰值计数 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 一次摇动事件快照 */
typedef struct {
    float    magnitude_g;   /* 事件期间观测到的最大高频加速度幅度 (g) */
    uint32_t tick;          /* 事件触发时的 FreeRTOS tick */
} shake_event_t;

/** 初始化摇动检测器 — 注册 DMP 加速度回调. 必须在 dmp_mpu_init() 返回后调用. */
void shake_detector_init(void);

/** 主循环轮询: 有新摇动事件返回 true 并填充 evt, 否则返回 false 且不修改 evt. */
bool shake_detector_poll(shake_event_t *evt);

/** 摇动事件刚发生 (消抖期内) — 供敲击检测抑制摇动起止摆动造成的短脉冲误判 */
bool shake_detector_is_active(void);

/** 窗口内已记录的振荡换向数 — 供敲击检测判断振荡是否进行中 (dmp_bg 同任务上下文, 无竞争) */
uint8_t shake_detector_flips(void);

/** 抑制检测 (如亮度条调节期间): 重力跟踪继续, 但不产生事件 */
void shake_detector_suppress(bool on);

#ifdef __cplusplus
}
#endif
