/** @file tap_detector.h @brief 敲击检测 — 20Hz 加速度脉冲识别 (单击/双击) */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 一次敲击事件快照 */
typedef struct {
    uint8_t  count;       /* 1=单击, 2=双击 */
    uint8_t  direction;   /* DMP_TAP_X_UP(1) / X_DOWN(2) / Y_UP(3) / Y_DOWN(4) / Z_UP(5) / Z_DOWN(6) */
    float    magnitude_g; /* 该次敲击脉冲的高频加速度幅度 (g, 20Hz 采样) */
    uint32_t tick;        /* 事件触发时的 FreeRTOS tick */
} tap_event_t;

/** 初始化敲击检测器 — 注册 20Hz 加速度回调. 必须在 dmp_mpu_init() 返回后调用. */
void tap_detector_init(void);

/** 主循环轮询: 有新敲击事件返回 true 并填充 evt, 否则返回 false 且不修改 evt. */
bool tap_detector_poll(tap_event_t *evt);

/** 抑制检测 (如亮度条调节期间): 重力跟踪继续, 但不产生事件 */
void tap_detector_suppress(bool on);

/** 触摸上下文更新 (主循环 1s 节拍): true = 手在设备上, 跳过静置前置并封顶阈值 */
void tap_detector_set_touched(bool touched);

/** 轮询被拒的"疑似敲击"峰值 (mg) 与原因 (0=低于阈值 1=静置不足) — 现场调参诊断用 */
bool tap_detector_poll_dbg(uint16_t *mag_mg, uint8_t *reason);

#ifdef __cplusplus
}
#endif
