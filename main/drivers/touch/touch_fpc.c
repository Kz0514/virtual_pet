/**
 * @file touch_fpc.c
 * @brief 12 通道 FPC 电容触摸传感器驱动（ESP32-S3 旧版 API）
 *
 * 布局：
 *   左侧  (1 通道):  GPIO2  (触摸通道 2)  — 功能键
 *   顶部  (5 通道):  GPIO3-7 (触摸通道 3-7) — 水平滑块
 *   右侧  (6 通道):  GPIO8-13 (触摸通道 8-13) — 垂直滑块
 *
 * 使用 ESP32-S3 触摸传感器，定时器驱动扫描（亮屏动态 50/20Hz，
 * 息屏由独立探针任务按 esp_timer 节拍探测）。
 * 触摸通道与 GPIO 编号一一对应（通道 0-13）。
 */

#include "board.h"
#include "touch_fpc.h"
#define CONFIG_TOUCH_SUPPRESS_LEGACY_WARNING 1
#include "driver/touch_sensor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include <string.h>
#include <math.h>

static const char *TAG = "touch_fpc";

/* 通道映射：索引 → touch_pad_t */
static const touch_pad_t s_pads[TOUCH_CH_COUNT] = {
    TOUCH_PAD_NUM2,  /* 0: GPIO2  — 左侧 */
    TOUCH_PAD_NUM3,  /* 1: GPIO3  — 顶部 0 */
    TOUCH_PAD_NUM4,  /* 2: GPIO4  — 顶部 1 */
    TOUCH_PAD_NUM5,  /* 3: GPIO5  — 顶部 2 */
    TOUCH_PAD_NUM6,  /* 4: GPIO6  — 顶部 3 */
    TOUCH_PAD_NUM7,  /* 5: GPIO7  — 顶部 4 */
    TOUCH_PAD_NUM8,  /* 6: GPIO8  — 右侧 0 */
    TOUCH_PAD_NUM9,  /* 7: GPIO9  — 右侧 1 */
    TOUCH_PAD_NUM10, /* 8: GPIO10 — 右侧 2 */
    TOUCH_PAD_NUM11, /* 9: GPIO11 — 右侧 3 */
    TOUCH_PAD_NUM12, /* 10: GPIO12 — 右侧 4 */
    TOUCH_PAD_NUM13, /* 11: GPIO13 — 右侧 5 */
};

/* 检测状态与判定不变式:
 * - ref  (限速基线): 每帧最多吃 RL, 只追环境漂移不追按压
 * - d_sm (delta 慢均值) + jit (噪声抖动 EMA): 自适应阈值 = jit*6,
 *   噪声大的环境自动抬阈 (幻触免疫), 安静环境回落到 MIN 下限
 *   (轻触恢复灵敏)。DC 偏移不进阈值 — 只被限速基线吸收,
 *   否则近场手常驻信号会抬阈误伤真实触摸
 * - touched 带滞回: 按下 d>thr, 释放 d<thr/2 — 边界不抖 */
typedef struct {
    int32_t ref[TOUCH_CH_COUNT];      /* 限速基线 */
    int32_t d_sm[TOUCH_CH_COUNT];     /* delta 慢均值 (抖动基准) */
    int32_t jit[TOUCH_CH_COUNT];      /* 噪声抖动 |d-d_sm| EMA (dev 钳 ±60) */
    int32_t thr[TOUCH_CH_COUNT];      /* 当前判定阈值 (jit*6 与 MIN 取大, 诊断用) */
    uint32_t raw[TOUCH_CH_COUNT];
    bool touched[TOUCH_CH_COUNT];
    int filtered[TOUCH_CH_COUNT];
    uint32_t touch_start_tick[TOUCH_CH_COUNT];
    uint32_t release_until[TOUCH_CH_COUNT]; /* 释放保活截止 (tick) — 滑动断续桥接 */
    float smooth_left;
    float smooth_top_pos;
    float smooth_right_pos;
    float raw_top_pos; /* 未平滑顶部质心 — 滑动检测用(平滑系数会低估位移) */
} touch_state_t;

static touch_state_t s_ts = {0};
static TimerHandle_t s_scan_timer = NULL;
static bool s_calibrated = false; /* 首次校准后开启防手指污染守卫 */

/* ── 检测参数 ── */
#define BASELINE_RL_POS 4            /* 正向限速/帧: 追环境漂移不吞按压 (慢按净余仍可触发) */
#define BASELINE_RL_NEG 64           /* 负向快追: 释放/回落快速归零, 防快速滑动重按 d
                                         从低处起跳丢判定; 负 d 无触发风险, 追快安全 */
#define TOUCH_MIN_TOP 300            /* 顶部/左键阈值下限 (须盖过近场手信号) */
#define TOUCH_MIN_RIGHT 220          /* 右侧阈值下限 (抖动自适应兜底) */
#define TOUCH_JIT_K 6                /* 抖动倍数: 抖动大环境自动抬阈盖过环境噪声峰值, 幻触免疫 */
#define JIT_MAX_DEV 60               /* 抖动输入钳制: dev=|d-d_sm| 超 60 截断 —
                                        jit 只反映环境噪声, 真实按压尖峰不得抬阈
                                        (无钳制单次按压即把通道阈值抬入数秒死区) */
#define THR_CLAMP 360                /* 阈值整体封顶: jit 稳态 ≤ JIT_MAX_DEV → thr ≤ 360 */
#define RELEASE_HYSTERESIS 2         /* 滞回: 释放阈值 = 按下阈值 / 2 */
#define RELEASE_KEEP_MS 60           /* 释放保活: 刚释放 60ms 内仍计 touched —
                                        桥接滑动腾空间隙/压力波动 (按 tick,
                                        50Hz=3帧 / 20Hz 空闲档同样生效)。
                                        只作用于滑条通道 (CH1-11), 左键不保活
                                        (保活拖长单击 hold → 500ms 轻点边界误判长按) */
#define ENV_SNAP_CHANNELS 11         /* 环境阶跃: >=11 通道同时越阈 (USB/供电瞬态全 12 通跳,
                                       手掌覆盖 ≤10 通道不误伤) → 基线快照+清判定 */

/* ── 动态刷新率 (四级档位) ──
 * 活动判定统一: 任一通道 |d| > 80 (近场/手接近) → 立即最高档。
 * - 亮屏活动 50Hz: 手势状态机消费端上限 (20ms LVGL 定时器驱动)
 * - 亮屏空闲 20Hz: 亮屏持 PM 锁禁睡, 只省定时器唤醒 + 扫描 CPU
 * - 息屏快探 20Hz: 100ms 轻掠需 ≥2 采样帧凑齐 2 连击去抖
 * - 息屏深闲 2Hz: 无活动 15s → 睡眠窗口 500ms, 主功耗收益点 */
#define PROBE_ACT_THR 80             /* 活动判定阈值 (近场/手接近) */
#define SCAN_ON_ACTIVE_MS 20         /* 亮屏活动 50Hz */
#define SCAN_ON_IDLE_MS 50           /* 亮屏空闲 20Hz */
#define SCAN_ON_IDLE_AFTER_MS 30000  /* 亮屏无活动多久降档 */
#define PROBE_FAST_MS 50             /* 息屏快探 20Hz */
#define PROBE_SLOW_MS 500            /* 息屏深闲 2Hz */
#define PROBE_SLOW_AFTER_MS 15000    /* 息屏无活动多久降档 */

static uint32_t s_last_activity = 0;       /* 最近活动时刻 (tick) */
static uint32_t s_probe_interval = PROBE_FAST_MS; /* 当前息屏探针间隔 */
static uint32_t s_last_scan = 0;           /* 上帧扫描时刻 — 限速频率补偿基准 */

/* 独立探针任务: 息屏期由周期 esp_timer (RTC 闹钟) 驱动 —
 * 硬性绑定轻睡唤醒节拍, 探针频率 = probe_interval, 与主循环负载解耦 */
static TaskHandle_t s_probe_task = NULL;
static esp_timer_handle_t s_probe_timer = NULL;
static volatile bool s_probe_enabled = false;
static volatile bool s_wake_pending = false;

static void touch_note_activity(void)
{
    s_last_activity = xTaskGetTickCount();
    s_probe_interval = PROBE_FAST_MS;
}

/** 当前息屏探针间隔 (main.c 据此调循环延迟 — 深闲时睡眠窗口 500ms) */
uint32_t touch_fpc_probe_interval_ms(void) { return s_probe_interval; }

/** 诊断: 当前各通道判定阈值 (jit*6 与 MIN 下限取大) — 排障抬阈/死区 */
void touch_fpc_get_thr(int *thr_out) { memcpy(thr_out, s_ts.thr, sizeof(s_ts.thr)); }

/* ── 自动校准 ── */
static bool touch_auto_calibrate(void)
{
    ESP_LOGI(TAG, "正在自动校准触摸基线…");
    uint64_t sum[TOUCH_CH_COUNT] = {0};

    /* 数学保护: raw 全 0 时校准会把 0 当基线 → 之后任意微小读数都
     * 越阈, 判定全 true — 首轮全 0 直接放弃, 保持现有基线 */
    uint32_t chk = 0;
    for (int i = 0; i < TOUCH_CH_COUNT; i++) {
        uint32_t v = 0;
        touch_pad_read_raw_data(s_pads[i], &v);
        chk += v;
    }
    if (chk == 0) {
        ESP_LOGW(TAG, "校准前检查: raw 全 0 — 放弃校准, 保持现有基线");
        return false;
    }

    for (int round = 0; round < 100; round++) {
        for (int i = 0; i < TOUCH_CH_COUNT; i++) {
            uint32_t val = 0;
            touch_pad_read_raw_data(s_pads[i], &val);
            sum[i] += val;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    for (int i = 0; i < TOUCH_CH_COUNT; i++) {
        int32_t mean = (int32_t)(sum[i] / 100);
        /* 防手指污染: 重校准时用户若按着设备, 平均被手指抬高, 直接覆盖
         * 基线 → 释放后 delta 恒负 → 触摸失灵。偏离超 2*MIN 的通道保留
         * 旧 ref, 靠限速基线自愈; 首次校准 (ref 全 0) 不适用此守卫。 */
        if (s_calibrated) {
            int32_t min_thr = (i > TOUCH_TOP_CH_COUNT) ? TOUCH_MIN_RIGHT : TOUCH_MIN_TOP;
            int32_t dev = mean - s_ts.ref[i];
            if (dev < 0) dev = -dev;
            if (dev > 2 * min_thr) {
                ESP_LOGW(TAG, "CH%d 校准跳过 (手指/偏移 %ld > %d)",
                         i, (long)dev, 2 * min_thr);
                continue;
            }
        }
        s_ts.ref[i] = mean;
        s_ts.d_sm[i] = 0;
        s_ts.jit[i] = 0;
    }
    s_calibrated = true;
    ESP_LOGI(TAG, "校准完成: CH0=%ld CH1=%ld CH2=%ld CH3=%ld CH4=%ld CH5=%ld "
                  "CH6=%ld CH7=%ld CH8=%ld CH9=%ld CH10=%ld CH11=%ld",
             (long)s_ts.ref[0], (long)s_ts.ref[1],
             (long)s_ts.ref[2], (long)s_ts.ref[3],
             (long)s_ts.ref[4], (long)s_ts.ref[5],
             (long)s_ts.ref[6], (long)s_ts.ref[7],
             (long)s_ts.ref[8], (long)s_ts.ref[9],
             (long)s_ts.ref[10], (long)s_ts.ref[11]);
    return true;
}

/* ── 息屏唤醒探针 (独立任务 + esp_timer 节拍) ──
 * 探针由周期 esp_timer (RTC 闹钟) 驱动 — 频率精确 = probe_interval
 * (快 50ms / 深闲 500ms), 与主循环负载解耦。
 * 连击判定: 2 命中须落 150ms 窗内且同通道 — 真按 ≥100ms 必 2-3 连中;
 * 漂移越阈命中相隔秒级, 凑不齐连击。
 * 免疫窗: USB/供电事件后 5s 不判唤醒; 通道数 >10 判环境态。 */
static uint8_t s_probe_hits = 0;
static uint32_t s_hit_tick = 0;   /* 末次命中时刻 — 连击 150ms 时序窗判据 */
static int8_t s_hit_ch = -1;      /* 末次命中主通道 — 连击必须同通道 */
static int8_t s_act_ch = -1;      /* 息屏活动主通道累积: 同通道 2 帧才活动 */
static TickType_t s_immune_until = 0; /* USB/供电事件免疫窗 */
#define PROBE_HIT_REQUIRED 2
#define PROBE_HIT_WINDOW_MS 150   /* 两命中必须在此窗内 — 漂移越阈(间隔秒级)凑不齐 */
#define PROBE_MAX_CHANNELS 10

/* ── 单发扫描 (定时器回调与息屏探针共用; 亮屏 50Hz / 息屏 20Hz-2Hz 动态) ──
 * sleep_path: 息屏探针路径 — 息屏后 PA 关断/面板睡眠的电磁耦合漂移
 * ~330/s, 追速必须盖过漂移率, 否则 d 净涨破阈假唤醒。
 * 亮屏路径保持 200/s (RL 加大会吞慢滑)。 */
static void touch_scan_once(bool sleep_path)
{
    uint32_t now = xTaskGetTickCount();
    int n_dev = 0;
    bool near = false;
    /* 息屏期判定不落盘: touched 一旦置位 → 限速追速冻结 → d 死锁高位 →
     * 假唤醒连击。唤醒判定由探针独立复判 (d>thr, 无保活) 承担, scan 只
     * 维护基线/抖动/阈值; 全息屏期追速恒执行 — 平台漂移被吃光, 尖峰当帧
     * 自回, 真按压速度远大于追速, 唤醒不受影响 */
    /* 免疫窗由探针端裁决 (touch_fpc_sleep_probe), 扫描侧不施加窗口 */

    /* 限速频率补偿: 基线 RL 是"每帧"限速, 低频下同一漂移追得慢 → d 逐帧
     * 净涨。rate_k = 距上帧 ms / 20ms (封顶 25), 亮屏追速恒 ≈200/s;
     * 息屏路径再 ×2 盖过息屏漂移 */
    uint32_t dt = (now >= s_last_scan) ? (now - s_last_scan) : 0;
    s_last_scan = now;
    uint32_t rate_k = dt / 20 + 1;
    if (rate_k > 25) rate_k = 25;
    uint32_t rl_k = rate_k * (sleep_path ? 2 : 1); /* 息屏 ×2: 追速盖过漂移不破阈;
                                                      再大吞揉搓, 破坏缓慢按压唤醒 */

    int8_t act_ch = -1; /* 息屏活动判定: |d| 最大通道 (跨 2 帧必须同通道) */
    int32_t act_d = -1;

    for (int i = 0; i < TOUCH_CH_COUNT; i++) {
        touch_pad_read_raw_data(s_pads[i], &s_ts.raw[i]);

        /* ESP32-S3: 触摸时 raw 值上升 (delta = raw - ref > 0) */
        int32_t d = (int32_t)s_ts.raw[i] - s_ts.ref[i];

        /* 漂移跟踪 (按通道门控): 只有本通道空闲才追, 手指按住左键不会
         * 冻结其他通道基线。限速吸收: 环境 DC 偏移 (近场/供电/温湿度)
         * 被限速吞掉, 按压 delta 突变净余量仍够触发; |d|<=RL 完全跟随
         * (亚 RL 漂移零滞后)。
         * 负向快追 (RL_NEG=64): 手指离开/环境回落快速归零, 负 d 无按压风险 —
         * 快速滑动"释放→重按"间隔短, 追不平则重按 d 从低处起跳丢判定 */
        if (!s_ts.touched[i]) {
            s_ts.d_sm[i] += (d - s_ts.d_sm[i]) >> 4;        /* d 慢均值 */
            int32_t dev = d - s_ts.d_sm[i];
            if (dev > JIT_MAX_DEV) dev = JIT_MAX_DEV;
            else if (dev < -JIT_MAX_DEV) dev = -JIT_MAX_DEV; /* 尖峰不得抬阈 */
            s_ts.jit[i] += ((dev < 0 ? -dev : dev) - s_ts.jit[i]) >> 3;
            int32_t rl_pos = (int32_t)BASELINE_RL_POS * (int32_t)rl_k;
            int32_t rl_neg = (int32_t)BASELINE_RL_NEG * (int32_t)rl_k;
            if (d > rl_pos)
                s_ts.ref[i] += rl_pos;
            else if (d < -rl_neg)
                s_ts.ref[i] -= rl_neg;
            else
                s_ts.ref[i] += d;
            if (s_ts.ref[i] < 0)
                s_ts.ref[i] = 0; /* 安全: raw 不可能为负 */
        }

        /* 自适应阈值: 抖动大环境自动抬阈免疫幻触, 安静环境回落 MIN 下限
         * 恢复轻触灵敏 — 阈值跟着环境噪声走 */
        int32_t thr = s_ts.jit[i] * TOUCH_JIT_K;
        int32_t min_thr = (i > TOUCH_TOP_CH_COUNT) ? TOUCH_MIN_RIGHT : TOUCH_MIN_TOP;
        if (thr < min_thr)
            thr = min_thr;
        if (thr > THR_CLAMP)
            thr = THR_CLAMP; /* 抖动抬阈封顶 (防按压污染 + 保滑动可用) */
        s_ts.thr[i] = thr;

        bool was = s_ts.touched[i];
        bool active = (d > thr) || (was && d > thr / RELEASE_HYSTERESIS); /* 滞回释放 */
        if (sleep_path)
            active = false; /* 息屏期不落盘判定 — 基线恒追速 (见函数头注释) */

        if (active) {
            s_ts.touched[i] = true;
            n_dev++;
            if (!was)
                s_ts.touch_start_tick[i] = now;
            s_ts.release_until[i] = now + pdMS_TO_TICKS(RELEASE_KEEP_MS); /* 每次激活续期保活 */
        } else {
            /* 释放保活: 滑条通道最近激活后 60ms 内仍维持 touched — 桥接滑动中
             * 手指腾空换电极/压力瞬时跌破滞回阈的间隙, 防质心骤失断触; 手指
             * 真离开 60ms 内释放, 迟滞无感。保活帧不计 n_dev (手掌拿开瞬间
             * 12 通道同归释放, 计数会误判为环境阶跃触发快照) */
            bool keep = (i > 0) && was && (int32_t)(now - s_ts.release_until[i]) < 0;
            s_ts.touched[i] = keep;
            if (!keep)
                s_ts.touch_start_tick[i] = 0;
            else
                continue; /* 保活帧: filtered/质心权重沿用离开前值 → 位置保持该通道
                              (保活期手指已在途中, 刷新会稀释权重导致质心漂移) */
        }
        s_ts.filtered[i] = (s_ts.filtered[i] * 3 + (d > 0 ? (int)d : 0)) / 4;

        /* 活动判定 (近场/手接近, 含按压前的近场信号): |d|>80 → 刷新活动
         * 时刻 → 动态刷新率立即回最高档。息屏 (sleep_path) 改同通道连续
         * 2 帧: 供电瞬态 (2-4s 一波通道间轮换) 单帧即越阈, 单帧计活动会
         * 持续续活使深闲档不可达; 瞬态轮换凑不齐同通道 2 帧, 真按必齐。
         * 亮屏路径保持单帧 (活动推进动画帧率) */
        int32_t ad = (d > 0) ? d : -d;
        if (ad > PROBE_ACT_THR) {
            if (!sleep_path)
                near = true;
            else if (ad > act_d) {
                act_d = ad;
                act_ch = (int8_t)i;
            }
        }
    }
    if (sleep_path) {
        if (act_ch >= 0 && act_ch == s_act_ch) {
            touch_note_activity();
            s_act_ch = -1; /* 已刷新 — 重累积; 持续按压仍每 2 帧续活 */
        } else {
            s_act_ch = act_ch;
        }
    } else if (near) {
        touch_note_activity();
    }

    /* 环境阶跃快照: >=11 通道同时越阈 = USB 拔插/供电瞬态 (全 12 通跳,
     * 持续 1-2s), 非触摸 — 基线快照到 raw, 清判定, 短免疫窗。
     * 手掌覆盖 ≤10 通道不误伤 */
    if (n_dev >= ENV_SNAP_CHANNELS) {
        for (int i = 0; i < TOUCH_CH_COUNT; i++) {
            s_ts.ref[i] = (int32_t)s_ts.raw[i];
            s_ts.touched[i] = false;
            s_ts.touch_start_tick[i] = 0;
            s_ts.d_sm[i] = 0;
            s_ts.jit[i] = 0;
        }
        s_immune_until = now + pdMS_TO_TICKS(2000);
        touch_note_activity();
    }

    /* 顶部滑块（通道 1-5）：加权质心 → -1..+1 */
    float t_w = 0, t_s = 0;
    for (int i = 1; i <= TOUCH_TOP_CH_COUNT; i++) {
        if (s_ts.touched[i]) {
            float w = (float)s_ts.filtered[i];
            t_w += w * (i - 1);
            t_s += w;
        }
    }
    float tp = (t_s > 0) ? (t_w / t_s) / (TOUCH_TOP_CH_COUNT - 1) : -1.0f;
    tp = tp * 2.0f - 1.0f;
    s_ts.raw_top_pos = tp;
    s_ts.smooth_top_pos = s_ts.smooth_top_pos * 0.7f + tp * 0.3f;

    /* 右侧滑块（通道 6-11）：加权质心 → -1..+1 */
    float r_w = 0, r_s = 0;
    for (int i = TOUCH_TOP_CH_COUNT + 1; i < TOUCH_CH_COUNT; i++) {
        if (s_ts.touched[i]) {
            float w = (float)s_ts.filtered[i];
            r_w += w * (i - TOUCH_TOP_CH_COUNT - 1);
            r_s += w;
        }
    }
    float rp = (r_s > 0) ? (r_w / r_s) / (TOUCH_RIGHT_CH_COUNT - 1) : -1.0f;
    rp = rp * 2.0f - 1.0f;
    s_ts.smooth_right_pos = s_ts.smooth_right_pos * 0.3f + rp * 0.7f; /* faster response */

    /* 左侧按钮（通道 0） */
    s_ts.smooth_left = s_ts.smooth_left * 0.7f + (s_ts.touched[0] ? 1.0f : 0.0f) * 0.3f;
}

/* ── 息屏期唤醒探针 ──
 * 复用扫描侧更新的基线/抖动阈值数据, 连击判定独立复判 (逐通道
 * d > thr, 无滞回无保活); USB/供电瞬态由免疫窗 (5s) 覆盖,
 * scan 侧环境快照服务亮屏期瞬态 */

bool touch_fpc_sleep_probe(void)
{
    touch_scan_once(true); /* 息屏路径: 追速 ×2 盖过 PA 关/面睡漂移源 */

    /* 动态刷新率 (息屏档): 距上次活动 >15s → 深闲 2Hz (睡眠窗口
     * 50→500ms, 主功耗收益); 任何活动 (近场/触摸/免疫事件) 已由
     * touch_scan_once 的 touch_note_activity 复位为快探 20Hz */
    if ((xTaskGetTickCount() - s_last_activity) * portTICK_PERIOD_MS >
        PROBE_SLOW_AFTER_MS)
        s_probe_interval = PROBE_SLOW_MS;
    else
        s_probe_interval = PROBE_FAST_MS;

    /* 免疫窗: USB 拔插/模式切换后 5s 内不判唤醒 — 瞬态 (VBUS
     * 消失/恢复, PHY 电源域切换) 使 raw 持续跳变 1-2s。
     * 手动唤醒路径独立, 不在此窗内 */
    if (xTaskGetTickCount() < s_immune_until) {
        s_probe_hits = 0;
        return false;
    }

    /* 连击判定独立复判 d > thr, 不用 s_ts.touched — touched 含 60ms 释放
     * 保活, 会把单帧环境尖峰撑成多帧计数在窗内凑齐连击 → 假唤醒。
     * 独立复判 (无滞回无保活): 尖峰单帧越阈只计 1 次, 真实按压 ≥100ms
     * 在 20Hz 下 ≥2 帧连续越阈必齐。通道数沿用 PROBE_MAX_CHANNELS
     * (手掌覆盖整体越阈不判) */
    int n_act = 0;
    for (int i = 0; i < TOUCH_CH_COUNT; i++) {
        int32_t d = (int32_t)s_ts.raw[i] - s_ts.ref[i];
        if (d > s_ts.thr[i])
            n_act++;
    }
    if (n_act == 0 || n_act > PROBE_MAX_CHANNELS) {
        s_probe_hits = 0;
        s_hit_ch = -1;
        return false;
    }

    /* 连击必须同通道: 供电瞬态跨通道轮换, 窗内异通道命中作废; 真实
     * 按压固定同通道, 不受影响。主通道 = 超阈通道中 d 最大者 */
    int8_t best_ch = -1;
    int32_t best_d = -INT32_MAX;
    for (int i = 0; i < TOUCH_CH_COUNT; i++) {
        int32_t d = (int32_t)s_ts.raw[i] - s_ts.ref[i];
        if (d > s_ts.thr[i] && d > best_d) {
            best_d = d;
            best_ch = (int8_t)i;
        }
    }

    /* 150ms 时序窗 + 通道一致性: 两命中相隔 >150ms 或换通道 → 重新起算
     * (漂移/瞬态轮换凑不齐); 真按 50ms 节拍同通道必齐 */
    uint32_t now_t = xTaskGetTickCount();
    if (s_probe_hits &&
        ((now_t - s_hit_tick) > pdMS_TO_TICKS(PROBE_HIT_WINDOW_MS) ||
         best_ch != s_hit_ch))
        s_probe_hits = 0;
    s_probe_hits++;
    s_hit_tick = now_t;
    s_hit_ch = best_ch;
    if (s_probe_hits < PROBE_HIT_REQUIRED)
        return false;

    s_probe_hits = 0;
    s_wake_pending = true; /* 唤醒标志 → main.c 消费 (屏幕状态/日志一体化) */
    return true;
}

/* 独立探针任务 + esp_timer 节拍:
 * esp_timer 是 RTC 闹钟: 周期 alarm 硬性约束轻睡唤醒节拍, 探针频率
 * 精确 = 配置值, 与主循环负载无关 (vTaskDelay/主循环阻塞拖不动它)。
 * 任务仅在息屏期 (s_probe_enabled) 运行探测, 亮屏期阻塞在通知上, 零开销。 */
static void probe_timer_cb(void *arg)
{
    BaseType_t hp = pdFALSE;
    xTaskNotifyFromISR(s_probe_task, 0, eNoAction, &hp);
    portYIELD_FROM_ISR(hp);
}

static void probe_task_fn(void *arg)
{
    uint32_t cur_iv_us = 0;
    for (;;) {
        /* 亮屏期: 等待使能 (pause → xTaskNotifyGive) */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (!s_probe_enabled) { /* pause 清标志后兜底再等 */
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            if (!s_probe_enabled) continue;
        }

        cur_iv_us = PROBE_FAST_MS * 1000;
        esp_timer_start_periodic(s_probe_timer, cur_iv_us);

        while (s_probe_enabled) {
            BaseType_t rc = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(600));
            (void)rc;
            if (!s_probe_enabled) break;
            touch_fpc_sleep_probe(); /* 内部按活动/深闲更新 s_probe_interval */
            uint32_t iv = s_probe_interval * 1000;
            if (iv != cur_iv_us) {  /* 快探↔深闲 档位切换 → 重设周期 */
                esp_timer_stop(s_probe_timer);
                esp_timer_start_periodic(s_probe_timer, iv);
                cur_iv_us = iv;
            }
        }
        esp_timer_stop(s_probe_timer);
        cur_iv_us = 0;
    }
}

/** main.c 消费: 探针 2 连击达成 → 置唤醒位, 主循环据此亮屏 (返回即清除) */
bool touch_fpc_wake_pending(void)
{
    if (!s_wake_pending) return false;
    s_wake_pending = false;
    return true;
}

/* USB/供电事件 → 5s 免疫窗 (见 touch_fpc.h 注释) */
void touch_fpc_note_usb_event(void)
{
    s_immune_until = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
    s_probe_hits = 0;
    touch_note_activity(); /* 免疫期仍保持快探档 (瞬态结束后立即恢复响应) */
}

/* 诊断: 去抖命中数 — power_log.csv ph 列 */
uint8_t touch_fpc_probe_hits(void) { return s_probe_hits; }

/* 诊断: 当前超阈值通道数 — power_log.csv tc 列 */
uint8_t touch_fpc_touched_count(void)
{
    uint8_t n = 0;
    for (int i = 0; i < TOUCH_CH_COUNT; i++)
        if (s_ts.touched[i]) n++;
    return n;
}

/* ── 周期性扫描定时器回调 (亮屏动态刷新率: 活动 50Hz / 空闲 20Hz) ──
 * 手势窗口余量: 20Hz 下数据最多陈旧 50ms — 释放边滞后 ≤50ms, 远小于
 * 最小手势窗口 (NAV_TAP_MAX_MS=500ms / NAV_SETTLE_MS=100ms); 触摸起始
 * 边活动判定 ≤50ms 内回 50Hz。LVGL indev 轮询只读最新状态, 无影响。
 * 亮屏持 PM 锁禁睡, 此档只省定时器唤醒 + 扫描 CPU — 适中即可 */
static void touch_scan_timer_cb(TimerHandle_t timer)
{
    touch_scan_once(false); /* 亮屏路径: 追速 200/s, 保慢滑余量 */

    uint32_t idle_ms = (xTaskGetTickCount() - s_last_activity) * portTICK_PERIOD_MS;
    TickType_t want = pdMS_TO_TICKS((idle_ms < SCAN_ON_IDLE_AFTER_MS)
                                        ? SCAN_ON_ACTIVE_MS
                                        : SCAN_ON_IDLE_MS);
    if (xTimerGetPeriod(s_scan_timer) != want)
        xTimerChangePeriod(s_scan_timer, want, 0); /* 回调内改周期: 队列延迟 1 tick, 安全 */
}

/* ── 公开 API ── */
esp_err_t touch_fpc_init(void)
{
    ESP_LOGI(TAG, "正在初始化 12 通道 FPC 触摸传感器（旧版 API）…");

    /* 初始化触摸外设: 硬件定时器自动触发测量 */
    touch_pad_init();
    touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER); /* 硬件自动触发 */
    touch_pad_fsm_start();

    /* 配置每个通道 */
    for (int i = 0; i < TOUCH_CH_COUNT; i++) {
        touch_pad_config(s_pads[i]);
        /* 设置电压阈值：参考电压的 2/3 = 中等灵敏度 */
        touch_pad_set_thresh(s_pads[i], 800);
    }

    /* 自动校准基线 */
    touch_auto_calibrate();

    /* 创建周期性扫描定时器（亮屏活动 50 Hz, 动态档由回调内切换） */
    s_scan_timer = xTimerCreate(
        "touch_scan",
        pdMS_TO_TICKS(SCAN_ON_ACTIVE_MS),
        pdTRUE, /* 自动重载 */
        NULL,
        touch_scan_timer_cb);
    if (!s_scan_timer) {
        ESP_LOGE(TAG, "创建扫描定时器失败");
        return ESP_ERR_NO_MEM;
    }
    s_last_activity = xTaskGetTickCount(); /* 启动即活动态 (50Hz 起步) */
    xTimerStart(s_scan_timer, 0);

    /* 独立探针任务 + esp_timer 节拍 (息屏期 20Hz/2Hz, 亮屏期阻塞零开销) */
    esp_timer_create_args_t targs = {
        .callback = probe_timer_cb,
        .arg = NULL,
        .name = "touch_probe",
    };
    if (esp_timer_create(&targs, &s_probe_timer) != ESP_OK)
        ESP_LOGE(TAG, "创建探针 esp_timer 失败");
    if (xTaskCreate(probe_task_fn, "touch_prb", 3072, NULL, 6, &s_probe_task)
        != pdPASS)
        ESP_LOGE(TAG, "创建探针任务失败");

    ESP_LOGI(TAG, "触摸 FPC 已初始化（12 通道, 动态刷新率 50/20Hz 亮屏 + 20/2Hz 息屏, 探针独立任务）");
    return ESP_OK;
}

void touch_fpc_scan(void)
{
    /* 空操作：扫描在定时器回调中完成。
     * 此函数保留以保持 API 兼容性。 */
}

/* ── 睡眠节电 ── */

void touch_fpc_pause(void)
{
    if (s_scan_timer) xTimerStop(s_scan_timer, 0);
    s_probe_hits = 0;
    s_wake_pending = false;
    /* 息屏瞬态免疫窗 (1500ms): PA 关断/面板睡眠/电源整定引起的瞬态
     * ~900ms 内衰尽, 窗口留 2× 余量, 窗内不判唤醒。手动唤醒 (左键/
     * 摇动/硬件触摸退出回调) 不经过探针免疫窗, 息屏即摸不受影响 */
    s_immune_until = xTaskGetTickCount() + pdMS_TO_TICKS(1500);
    touch_note_activity(); /* 屏刚灭 = 快探档起步, 15s 无活动才降深闲 */

    /* 启动独立探针任务 (esp_timer 20Hz 节拍 — 见 probe_task_fn) */
    s_probe_enabled = true;
    if (s_probe_task) xTaskNotifyGive(s_probe_task);
}

void touch_fpc_resume(void)
{
    /* 亮屏唤醒 = 刷新基线基准: 睡眠期供电/耦合漂移会让软件判定
     * (raw vs ref) 失真 — 重校准后首轮扫描即新基线, 判定恢复。
     * 校准 500ms 期间先停扫描定时器 (防并发读写基线), 校准完再
     * 恢复扫描。校准内置 raw 全 0 保护 + 手指污染守卫 (手指压着的
     * 通道保留旧基线, 靠限速基线自愈 — 唤醒触摸不得污染基线) */
    if (s_scan_timer) xTimerStop(s_scan_timer, 0);
    s_probe_hits = 0;
    s_wake_pending = false;
    s_probe_enabled = false; /* 停独立探针任务 (任务内 ≤600ms 收尾停 esp_timer) */
    touch_auto_calibrate();
    touch_note_activity();
    if (s_scan_timer) {
        xTimerChangePeriod(s_scan_timer, pdMS_TO_TICKS(SCAN_ON_ACTIVE_MS), 0);
        xTimerStart(s_scan_timer, 0);
    }
}

bool touch_fpc_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    lv_coord_t x = (lv_coord_t)((s_ts.smooth_top_pos + 1.0f) / 2.0f * DISPLAY_WIDTH);
    lv_coord_t y = (lv_coord_t)((s_ts.smooth_right_pos + 1.0f) / 2.0f * DISPLAY_HEIGHT);

    data->point.x = x;
    data->point.y = y;
    data->state = (s_ts.touched[0] || (fabsf(s_ts.smooth_top_pos) > 0.1f) || (fabsf(s_ts.smooth_right_pos) > 0.1f))
                      ? LV_INDEV_STATE_PRESSED
                      : LV_INDEV_STATE_RELEASED;
    return false;
}

/* ── 公开查询接口 ── */
bool touch_is_left_pressed(void) { return s_ts.touched[0]; }
uint32_t touch_left_hold_ms(void) { return s_ts.touched[0] ? (xTaskGetTickCount() - s_ts.touch_start_tick[0]) * portTICK_PERIOD_MS : 0; }
float touch_top_position(void) { return s_ts.smooth_top_pos; }
float touch_top_position_raw(void) { return s_ts.raw_top_pos; }
float touch_right_position(void) { return s_ts.smooth_right_pos; }

bool touch_is_top_pressed(void)
{
    /* 顶部滑条 5 通道 (索引 1-5) 任一被触摸 */
    for (int i = 1; i <= TOUCH_TOP_CH_COUNT; i++) {
        if (s_ts.touched[i]) return true;
    }
    return false;
}

bool touch_is_right_pressed(void)
{
    /* Check if any right-side channel (6-11) is actually touched */
    for (int i = TOUCH_TOP_CH_COUNT + 1; i < TOUCH_CH_COUNT; i++) {
        if (s_ts.touched[i]) return true;
    }
    return false;
}

bool touch_is_petting_head(void)
{
    int n = 0;
    for (int i = 1; i <= TOUCH_TOP_CH_COUNT; i++) {
        if (s_ts.touched[i]) n++;
    }
    if (n < 3) return false;
    uint32_t ms = UINT32_MAX;
    for (int i = 1; i <= TOUCH_TOP_CH_COUNT; i++) {
        if (s_ts.touched[i] && s_ts.touch_start_tick[i] < ms) ms = s_ts.touch_start_tick[i];
    }
    return (xTaskGetTickCount() - ms) * portTICK_PERIOD_MS > 500;
}

bool touch_is_top_middle_pressed(void)
{
    /* 顶部通道 1-5 的中间三个 (GPIO4/5/6 = 通道索引 2-4) */
    for (int i = 2; i <= 4; i++) {
        if (s_ts.touched[i]) return true;
    }
    return false;
}

void touch_get_raw(uint32_t *out) { memcpy(out, s_ts.raw, sizeof(s_ts.raw)); }
void touch_get_baseline(uint32_t *out)
{
    /* 基线 = ref (限速基线, 恒非负 — 见 touch_scan_once 钳位) */
    for (int i = 0; i < TOUCH_CH_COUNT; i++)
        out[i] = (uint32_t)s_ts.ref[i];
}
void touch_get_filtered(int *out) { memcpy(out, s_ts.filtered, sizeof(s_ts.filtered)); }
