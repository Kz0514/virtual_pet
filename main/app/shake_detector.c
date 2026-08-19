/** @file shake_detector.c @brief 摇动检测 — 重力高通 + 主导轴换向计数 (DMP 20Hz 加速度流)
 * 判据: 高频加速度主导轴的符号翻转 = 半个振荡周期, 窗口内翻够次数即触发.
 * 相比幅度峰值计数, 换向判据对幅度起伏免疫 (剧烈摇动时两摆之间幅度掉不到迟滞下沿也不会漏),
 * 且天然抵抗单向敲击噪声 (敲击方向固定, 不会翻转符号). */
#include "shake_detector.h"
#include "dmp_mpu.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

static const char *TAG = "shake";

/* ── 调参 (基于 40Hz DMP FIFO 速率, 25ms/样本) ── */
#define SD_LP_ALPHA         0.05f   /* 重力跟踪低通系数 (τ≈0.5s @40Hz) */
#define SD_FLIP_ARM_G       0.18f   /* 换向计数的幅度门限 (g) — 低于此视为静止 (换向判据抗噪声, 可下探) */
#define SD_FLIPS_NEEDED     4       /* 触发所需换向数 = 2 个完整振荡周期 */
#define SD_FLIP_WINDOW      48      /* 换向窗口 1.2s × 40Hz (按样本计时: FIFO 包固定间隔 25ms) */
#define SD_FLIP_SPACING     4       /* 相邻换向最小间隔 (样本, 100ms, 防信号弹跳) */
#define SD_DEBOUNCE_SAMPLES 40      /* 事件后消抖 1s × 40Hz */
#define SD_LSB_PER_G        16384.0f /* ±2g — 若改 mpu_init 量程需同步 */

static float    s_lp[3];                    /* 逐轴重力估计 */
static bool     s_lp_init;                  /* 首样本已播种 */
static volatile bool s_suppress = false;    /* 抑制 (亮度条调节期间) */
static int8_t   s_sign;                     /* 主导轴当前符号 (+1/-1), 0=静止 */
static uint8_t  s_flip_age[SD_FLIPS_NEEDED];/* 换向年龄 (样本数), 超过窗口即丢弃 */
static uint8_t  s_flip_cnt;
static float    s_window_max;               /* 当前换向序列内最大 HP 幅度 */
static uint16_t s_debounce;                 /* 事件后消抖剩余样本数 */

/* 事件快照: 先写数据, 再原子递增计数 (32位对齐, Xtensa 原子) */
static volatile float    s_event_mag;
static volatile uint32_t s_event_tick;
static volatile uint32_t s_event_count;
static uint32_t s_last_count;               /* 单消费者 (主循环), 无需保护 */

/** 加速度回调 — 运行于 dmp_bg 任务上下文: 必须短小, 禁止 I2C/阻塞/浮点日志 */
static void accel_cb(short ax, short ay, short az)
{
    float x = ax / SD_LSB_PER_G, y = ay / SD_LSB_PER_G, z = az / SD_LSB_PER_G;

    if (!s_lp_init) {
        /* 首样本直接作为重力基准, 避免初始化瞬态 */
        s_lp[0] = x; s_lp[1] = y; s_lp[2] = z;
        s_lp_init = true;
        return;
    }

    /* 重力跟踪 (消抖期间也持续更新, 防止基准漂移; 40Hz 采样 τ≈0.5s) */
    s_lp[0] += SD_LP_ALPHA * (x - s_lp[0]);
    s_lp[1] += SD_LP_ALPHA * (y - s_lp[1]);
    s_lp[2] += SD_LP_ALPHA * (z - s_lp[2]);

    float hx = x - s_lp[0], hy = y - s_lp[1], hz = z - s_lp[2];
    float mag = sqrtf(hx*hx + hy*hy + hz*hz);

    if (s_suppress) return;   /* 抑制期间只跟踪重力 */

    /* 消抖期间只跟踪重力 */
    if (s_debounce) { s_debounce--; return; }

    /* 换向年龄老化, 超过窗口的丢弃 (压缩数组) */
    uint8_t n = 0;
    for (int i = 0; i < s_flip_cnt; i++) {
        if (++s_flip_age[i] > SD_FLIP_WINDOW) continue;
        s_flip_age[n++] = s_flip_age[i];
    }
    s_flip_cnt = n;

    if (mag < SD_FLIP_ARM_G) {
        /* 低于门限视为静止, 重置符号 — 下次抬起时从头判向 */
        s_sign = 0;
        return;
    }

    if (mag > s_window_max) s_window_max = mag;

    /* 主导轴: 高频分量最大的轴 */
    float dom = hx;
    if (fabsf(hy) > fabsf(dom)) dom = hy;
    if (fabsf(hz) > fabsf(dom)) dom = hz;
    int8_t sign = (dom > 0) ? 1 : -1;

    /* 符号翻转 = 半个振荡周期 */
    if (s_sign != 0 && sign != s_sign) {
        uint8_t last_age = (s_flip_cnt > 0) ? s_flip_age[s_flip_cnt - 1] : 0xFF;
        if (last_age == 0xFF || last_age >= SD_FLIP_SPACING) {
            if (s_flip_cnt < SD_FLIPS_NEEDED) {
                if (s_flip_cnt == 0) s_window_max = mag;  /* 新序列起点, 清掉陈旧最大值 */
                s_flip_age[s_flip_cnt++] = 0;
            }
            if (s_flip_cnt >= SD_FLIPS_NEEDED) {
                /* 触发事件: 先写数据, 再递增计数 */
                s_event_mag  = s_window_max;
                s_event_tick = xTaskGetTickCount();
                s_event_count++;
                /* 干净复位 */
                s_sign       = 0;
                s_flip_cnt   = 0;
                s_window_max = 0.0f;
                s_debounce   = SD_DEBOUNCE_SAMPLES;
            }
        }
    }
    s_sign = sign;
}

void shake_detector_suppress(bool on) { s_suppress = on; }

void shake_detector_init(void)
{
    s_lp_init    = false;
    s_suppress   = false;
    s_sign       = 0;
    s_flip_cnt   = 0;
    s_window_max = 0.0f;
    s_debounce   = 0;
    s_event_count = 0;
    s_last_count  = 0;

    dmp_mpu_add_accel_cb(accel_cb);
    ESP_LOGI(TAG, "摇动检测器就绪 (≥%d次换向/%.1fs, 幅度门限%.2fg)",
             SD_FLIPS_NEEDED, SD_FLIP_WINDOW * 0.05f, SD_FLIP_ARM_G);
}

bool shake_detector_poll(shake_event_t *evt)
{
    uint32_t n = s_event_count;
    if (n == s_last_count) return false;
    s_last_count = n;
    if (evt) {
        evt->magnitude_g = s_event_mag;
        evt->tick        = s_event_tick;
    }
    return true;
}

/** 摇动事件刚发生 (消抖期内) — 供 tap_detector 抑制敲击误判 */
bool shake_detector_is_active(void)
{
    return s_debounce > 0;
}

/** 窗口内已记录的换向数 — 供 tap_detector 判断振荡是否进行中 (dmp_bg 同任务上下文, 无竞争) */
uint8_t shake_detector_flips(void)
{
    return s_flip_cnt;
}
