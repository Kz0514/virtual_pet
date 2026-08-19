/** @file tap_detector.c @brief 敲击检测 — 20Hz 原始加速度流上的脉冲识别 (单击/双击)
 * 弃用 DMP 内置 tap 特征 (本项目配置下无物理依据地频繁幻报, 调阈值无效),
 * 改为在 dmp_bg 的 20Hz 加速度回调里自研: 短脉冲计数 + 双击分组, 方向由高频向量主分量推算. */
#include "tap_detector.h"
#include "dmp_mpu.h"
#include "shake_detector.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

static const char *TAG = "tap";

/* ── 调参 (基于 40Hz 采样 = 25ms/样本, 环境背景噪声实测 0.02~0.24g, 实测真敲击 0.18~1.57g) ── */
#define TD_TAP_ARM_MIN       0.10f   /* 自适应阈值下限 (g) — 轻敲实测 0.10~0.14g 必须能过 */
#define TD_TOUCH_ARM_MAX     0.15f   /* 手持期间自适应阈值上限 (手部微颤会抬升基线, 需封顶) */
#define TD_TOUCH_QUIET_BASE  0.05f   /* 手持且环境基线低于此值才跳过静置前置 (滑动会抬升基线) */
#define TD_ARM_RATIO         3.0f    /* 首脉冲阈值 = max(下限, 3×近1s环境基线) */
#define TD_TAP_ARM2_G        0.30f   /* 双击第二脉冲阈值下限 (再取 max(自适应)) —
                                      * 手搭设备/桌面震动会幻报双脉冲, 0.20g 误判多 */
#define TD_TAP_HOLD_G        0.07f   /* 脉冲持续判定下沿 (上沿 50%) */
#define TD_QUIET_G           0.10f   /* 静止判定 (滑动中的摩擦颤振会高于此线) */
#define TD_MIN_QUIET         16      /* 首脉冲前置静置 400ms — 排除滑动粘滑, 且不吞连续敲击 */
#define TD_RING_SAMPLES      40      /* 环境基线窗口 1s */
#define TD_PULSE_MAX_SAMPLES 8       /* 脉冲最长 200ms, 超过视为摇动/推动而非敲击 */
#define TD_DOUBLE_WINDOW     28      /* 双击分组窗口 700ms */
#define TD_COOLDOWN_MS       1200    /* 事件组间最小间隔 (须 < 双击窗口 + 单击延迟, 否则吃第二下) */
#define TD_LSB_PER_G         16384.0f

/* 事件快照: 先写数据, 再原子递增序号 (32位对齐, Xtensa 原子) */
static volatile uint8_t  s_ev_count;
static volatile uint8_t  s_ev_direction;
static volatile float    s_ev_mag;
static volatile uint32_t s_ev_tick;
static volatile uint32_t s_ev_seq;
static uint32_t s_last_seq;             /* 单消费者 (主循环), 无需保护 */
static uint32_t s_last_cb_tick;         /* 冷却 */

/* 重力跟踪 */
static float s_lp[3];
static bool  s_lp_init;

/* 抑制 (亮度条调节期间): 重力跟踪继续, 事件检测暂停 */
static volatile bool s_suppress = false;

/* 触摸上下文 (主循环 1s 节拍更新): 手持时手部微动让静置窗口攒不满,
 * 且桌面滑动不可能发生 — 触摸时跳过静置前置并限制自适应阈值 */
static volatile bool s_touched = false;

/* 脉冲检测 */
static bool    s_pulse_active;
static uint8_t s_pulse_len;
static float   s_pulse_mag;
static uint8_t s_pulse_dir;
static uint8_t s_quiet;                 /* 连续静止样本数 (敲击前置条件) */

/* 环境基线: 近 1s 幅度环形窗口 (自适应阈值用) */
static float   s_ring[TD_RING_SAMPLES];
static uint8_t s_ring_idx;

/* 诊断: 亚阈值"疑似敲击"的峰值 (mg) 与拒绝原因, 主循环轮询打印 */
static volatile uint16_t s_dbg_mag_mg;
static volatile uint8_t  s_dbg_reason;   /* 0=低于阈值 1=静置不足 */
static volatile uint32_t s_dbg_seq;
static uint32_t s_dbg_last_seq;
static float   s_sub_peak;
static uint8_t s_sub_reason;
static bool    s_sub_active;

/* 双击分组: 脉冲先挂起, 500ms 静默后按组内脉冲数上报; 第二脉冲升级为双击候选 */
static bool    s_pending;
static uint8_t s_pending_count;
static float   s_pending_mag;
static uint8_t s_pending_dir;
static uint8_t s_pending_cntdown;

/** 高频加速度向量主分量 → DMP 方向编号 (1..6, 与 dmp_mpu.h 的 DMP_TAP_* 一致) */
static uint8_t dominant_dir(float hx, float hy, float hz)
{
    float ax = fabsf(hx), ay = fabsf(hy), az = fabsf(hz);
    if (ax >= ay && ax >= az) return hx > 0 ? 1 : 2;   /* X_UP / X_DOWN */
    if (ay >= az)             return hy > 0 ? 3 : 4;   /* Y_UP / Y_DOWN */
    return hz > 0 ? 5 : 6;                             /* Z_UP / Z_DOWN */
}

static void emit(uint8_t count, uint8_t dir, float mag, uint32_t tick)
{
    s_ev_count     = count;
    s_ev_direction = dir;
    s_ev_mag       = mag;
    s_ev_tick      = tick;
    s_ev_seq++;
    s_last_cb_tick = tick;
}

/** 一个脉冲结束且长度合格 → 进入双击分组 (dmp_bg 上下文, 保持短小) */
static void on_pulse(float mag, uint8_t dir)
{
    /* 摇动事件刚发生 (消抖期内) 或振荡进行中 (≥2 次换向) 时不计敲击 —
     * 摇动的起止摆动会被切成短脉冲, 需用换向数区分真敲击 */
    if (shake_detector_is_active() || shake_detector_flips() >= 2) return;
    if (xTaskGetTickCount() - s_last_cb_tick < pdMS_TO_TICKS(TD_COOLDOWN_MS)) return;

    if (s_pending) {
        s_pending_count = 2;                    /* 升级为双击候选 */
        if (mag > s_pending_mag) s_pending_mag = mag;
        s_pending_cntdown = TD_DOUBLE_WINDOW;   /* 重新计时 */
    } else {
        s_pending = true;
        s_pending_count = 1;
        s_pending_mag = mag;
        s_pending_dir = dir;
        s_pending_cntdown = TD_DOUBLE_WINDOW;
    }
}

/** 加速度回调 — dmp_bg 任务上下文: 必须短小, 禁止 I2C/阻塞/浮点日志 */
static void accel_cb(short ax, short ay, short az)
{
    float x = ax / TD_LSB_PER_G, y = ay / TD_LSB_PER_G, z = az / TD_LSB_PER_G;

    if (!s_lp_init) {
        s_lp[0] = x; s_lp[1] = y; s_lp[2] = z;   /* 首样本即重力基准 */
        s_lp_init = true;
        return;
    }
    s_lp[0] += 0.05f * (x - s_lp[0]);   /* 40Hz 采样, τ≈0.5s */
    s_lp[1] += 0.05f * (y - s_lp[1]);
    s_lp[2] += 0.05f * (z - s_lp[2]);

    float hx = x - s_lp[0], hy = y - s_lp[1], hz = z - s_lp[2];
    float mag = sqrtf(hx*hx + hy*hy + hz*hz);

    if (s_suppress) return;   /* 抑制期间只跟踪重力 */

    /* 环境自适应阈值: 近 1s 最小幅度 × 3 (下限 0.10g)。
     * 静置时基线 ~0.02g → 阈值 0.10g, 轻敲可过;
     * 桌面滑动时摩擦颤振抬升基线 → 阈值升高, 粘滑抖动被拒 */
    float mn = 10.0f;
    for (int i = 0; i < TD_RING_SAMPLES; i++) {
        if (s_ring[i] < mn) mn = s_ring[i];
    }
    float arm_eff = mn * TD_ARM_RATIO;
    if (arm_eff < TD_TAP_ARM_MIN) arm_eff = TD_TAP_ARM_MIN;
    if (s_touched && arm_eff > TD_TOUCH_ARM_MAX) arm_eff = TD_TOUCH_ARM_MAX;
    /* 首脉冲: 自适应阈值; 组内第二脉冲: 需更强 (防滑动粘滑对) */
    float arm = s_pending ? (TD_TAP_ARM2_G > arm_eff ? TD_TAP_ARM2_G : arm_eff)
                          : arm_eff;

    /* 双击窗口倒计时: 静默 700ms 后按组内脉冲数上报 */
    if (s_pending && --s_pending_cntdown == 0) {
        if (!shake_detector_is_active() && shake_detector_flips() < 2) {
            emit(s_pending_count, s_pending_dir, s_pending_mag, xTaskGetTickCount());
        }
        s_pending = false;      /* 振荡进行中则丢弃该组 */
    }

    if (!s_pulse_active) {
        /* 手在设备上且环境基线安静时跳过静置前置 (连敲时桌面余振攒不满静置窗口);
         * 桌面滑动时摩擦颤振抬升基线 → 静置门槛自动保留, 防误报不被破坏 */
        bool quiet_ok = s_quiet >= TD_MIN_QUIET ||
                        (s_touched && mn < TD_TOUCH_QUIET_BASE);
        if (mag > arm && (s_pending || quiet_ok)) {
            s_pulse_active = true;
            s_pulse_len = 1;
            s_pulse_mag = mag;
            s_pulse_dir = dominant_dir(hx, hy, hz);
        }
    } else {
        if (mag > TD_TAP_HOLD_G) {
            s_pulse_len++;
            if (mag > s_pulse_mag) s_pulse_mag = mag;
        } else {
            s_pulse_active = false;
            if (s_pulse_len <= TD_PULSE_MAX_SAMPLES)
                on_pulse(s_pulse_mag, s_pulse_dir);
            /* 过长的脉冲不是敲击, 静默丢弃 */
        }
    }

    /* 亚阈值峰跟踪: 记录成不了脉冲的"疑似敲击"峰值与原因, 供现场调参 */
    if (!s_pulse_active) {
        if (mag > 0.05f) {
            s_sub_active = true;
            if (mag > s_sub_peak) {
                s_sub_peak = mag;
                s_sub_reason = (mag > arm) ? 1 : 0;   /* 超阈值却被拒 → 静置不足 */
            }
        } else if (s_sub_active) {
            s_sub_active = false;
            if (s_sub_peak >= 0.08f) {
                s_dbg_mag_mg = (uint16_t)(s_sub_peak * 1000.0f);
                s_dbg_reason = s_sub_reason;
                s_dbg_seq++;
            }
            s_sub_peak = 0.0f;
        }
    } else {
        s_sub_active = false;
        s_sub_peak = 0.0f;
    }

    /* 静止计数 (放在末尾: 判定读的是"本样本之前"的静置历史,
     * 敲击脉冲自身的强样本会清零计数, 但不能影响脉冲起判) */
    if (mag < TD_QUIET_G) {
        if (s_quiet < 0xFF) s_quiet++;
    } else {
        s_quiet = 0;
    }

    /* 环境基线环形窗口更新 */
    s_ring[s_ring_idx] = mag;
    s_ring_idx = (s_ring_idx + 1) % TD_RING_SAMPLES;
}

void tap_detector_suppress(bool on) { s_suppress = on; }

/** 触摸上下文更新 (主循环 1s 节拍): true = 手在设备上 */
void tap_detector_set_touched(bool touched) { s_touched = touched; }

void tap_detector_init(void)
{
    s_lp_init      = false;
    s_pulse_active = false;
    s_pending      = false;
    s_quiet        = TD_MIN_QUIET;   /* 开机视为已静置 */
    s_suppress     = false;
    s_ring_idx     = 0;
    memset(s_ring, 0, sizeof(s_ring));
    s_sub_peak   = 0.0f;
    s_sub_active = false;
    s_dbg_seq    = 0;
    s_dbg_last_seq = 0;
    s_ev_seq       = 0;
    s_last_seq     = 0;
    s_last_cb_tick = 0;

    dmp_mpu_add_accel_cb(accel_cb);
    ESP_LOGI(TAG, "敲击检测器就绪 (40Hz脉冲识别, 自适应阈值≥%.2fg, 单击/双击)", TD_TAP_ARM_MIN);
}

bool tap_detector_poll(tap_event_t *evt)
{
    uint32_t n = s_ev_seq;
    if (n == s_last_seq) return false;
    s_last_seq = n;
    if (evt) {
        evt->count       = s_ev_count;
        evt->direction   = s_ev_direction;
        evt->magnitude_g = s_ev_mag;
        evt->tick        = s_ev_tick;
    }
    return true;
}

/** 轮询被拒的"疑似敲击"峰值 (mg) 与原因 (0=低于阈值 1=静置不足) — 现场调参诊断用 */
bool tap_detector_poll_dbg(uint16_t *mag_mg, uint8_t *reason)
{
    uint32_t n = s_dbg_seq;
    if (n == s_dbg_last_seq) return false;
    s_dbg_last_seq = n;
    if (mag_mg) *mag_mg = s_dbg_mag_mg;
    if (reason) *reason = s_dbg_reason;
    return true;
}
