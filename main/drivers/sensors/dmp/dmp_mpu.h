/**
 * MPU6500 DMP wrapper — init, load firmware, read quaternion + pedometer.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
 float pitch; /* degrees, -90..+90 */
 float roll; /* degrees, -180..+180 */
 float yaw; /* degrees, 0..360 */
} dmp_angles_t;

/** Initialize I2C, MPU6500, load DMP firmware, enable features.
 * Call once at boot. */
esp_err_t dmp_mpu_init(void);

/** Read latest pitch/roll/yaw from DMP. Non-blocking, returns ESP_OK or fail. */
esp_err_t dmp_mpu_get_angles(dmp_angles_t *angles);

/** Get step count from DMP pedometer. */
unsigned long dmp_mpu_get_steps(void);

/** 息屏降频: off=true 时后台轮询 50→s_off_poll_ms (shake/tap 消费者息屏已 gate) */
void dmp_mpu_set_off(bool off);

/** 息屏 off 模式轮询间隔 (ms, 默认 250)。触摸深闲档 (2Hz) 调 500 减唤醒,
 * 快探档 (20Hz) 调回 250 — 跟随 touch_fpc_probe_interval_ms() 节奏。 */
void dmp_mpu_set_off_interval(uint32_t ms);

/** Reset step count. */
void dmp_mpu_reset_steps(void);

/* ── Tap detection ── */
/** Tap direction (TAP_X_UP=1, TAP_X_DOWN=2, ... TAP_Z_DOWN=6) */
#define DMP_TAP_X_UP 1
#define DMP_TAP_X_DOWN 2
#define DMP_TAP_Y_UP 3
#define DMP_TAP_Y_DOWN 4
#define DMP_TAP_Z_UP 5
#define DMP_TAP_Z_DOWN 6

/** @param direction one of DMP_TAP_* (回调第一个参数实际是方向, 第二个是次数)
 * @param count 1=single tap, 2=double tap
 * NOTE: called from DMP bg task — keep short! */
void dmp_mpu_register_tap_cb(void (*cb)(unsigned char direction, unsigned char count));

/* ── Screen orientation ── */
#define DMP_ORIENT_PORTRAIT 0
#define DMP_ORIENT_LANDSCAPE 1
#define DMP_ORIENT_REVERSE_PORTRAIT 2
#define DMP_ORIENT_REVERSE_LANDSCAPE 3

/** @param orientation one of DMP_ORIENT_*. NOTE: called from DMP bg task — keep short! */
void dmp_mpu_register_orient_cb(void (*cb)(unsigned char orientation));

/* ── Raw accelerometer ── */
/** Raw accel from each DMP FIFO packet (~20Hz), ±2g = 16384 LSB/g (post-mpu_init).
 * Up to DMP_MAX_ACCEL_CBS consumers; duplicate registration is a no-op.
 * NOTE: called from DMP bg task — keep short, no I2C, no float logging! */
#define DMP_MAX_ACCEL_CBS 4
void dmp_mpu_add_accel_cb(void (*cb)(short ax, short ay, short az));

#ifdef __cplusplus
}
#endif
