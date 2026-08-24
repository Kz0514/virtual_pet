/**
 * @file touch_fpc.c
 * @brief 12 通道 FPC 电容触摸传感器驱动（ESP32-S3 旧版 API）
 *
 * 布局：
 *   左侧  (1 通道):  GPIO2  (触摸通道 2)  — 功能键
 *   顶部  (5 通道):  GPIO3-7 (触摸通道 3-7) — 水平滑块
 *   右侧  (6 通道):  GPIO8-13 (触摸通道 8-13) — 垂直滑块
 *
 * 使用 ESP32-S3 触摸传感器，轮询模式（通过定时器以 50 Hz 扫描）。
 * 触摸通道与 GPIO 编号一一对应（通道 0-13）。
 */

#include "board.h"
#include "touch_fpc.h"
#define CONFIG_TOUCH_SUPPRESS_LEGACY_WARNING 1
#include "driver/touch_sensor.h"
#include "esp_log.h"
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

/* 状态 */
typedef struct {
    uint32_t baseline[TOUCH_CH_COUNT];
    uint32_t raw[TOUCH_CH_COUNT];
    bool touched[TOUCH_CH_COUNT];
    int filtered[TOUCH_CH_COUNT];
    uint32_t touch_start_tick[TOUCH_CH_COUNT];
    float smooth_left;
    float smooth_top_pos;
    float smooth_right_pos;
    float raw_top_pos; /* 未平滑顶部质心 — 滑动检测用(平滑系数会低估位移) */
} touch_state_t;

static touch_state_t s_ts = {0};
static TimerHandle_t s_scan_timer = NULL;

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
        s_ts.baseline[i] = (uint32_t)(sum[i] / 100);
    }
    ESP_LOGI(TAG, "校准完成: CH0=%lu CH1=%lu CH2=%lu CH3=%lu CH4=%lu CH5=%lu "
                  "CH6=%lu CH7=%lu CH8=%lu CH9=%lu CH10=%lu CH11=%lu",
             (unsigned long)s_ts.baseline[0], (unsigned long)s_ts.baseline[1],
             (unsigned long)s_ts.baseline[2], (unsigned long)s_ts.baseline[3],
             (unsigned long)s_ts.baseline[4], (unsigned long)s_ts.baseline[5],
             (unsigned long)s_ts.baseline[6], (unsigned long)s_ts.baseline[7],
             (unsigned long)s_ts.baseline[8], (unsigned long)s_ts.baseline[9],
             (unsigned long)s_ts.baseline[10], (unsigned long)s_ts.baseline[11]);
    return true;
}

/* ── 单发扫描 (定时器回调与 10Hz 兜底轮询共用) ── */
static void touch_scan_once(void)
{
    uint32_t now = xTaskGetTickCount();
    bool any_active = false;

    for (int i = 0; i < TOUCH_CH_COUNT; i++) {
        touch_pad_read_raw_data(s_pads[i], &s_ts.raw[i]);

        /* ESP32-S3: 触摸时 raw 值上升 (delta = raw - baseline > 0) */
        int32_t delta = (int32_t)s_ts.raw[i] - (int32_t)s_ts.baseline[i];
        /* Right-side channels (6-11) have smaller electrodes → lower threshold.
         * 300 (2026-08-23): 息屏 PA 关断后残余电磁耦合仍可使右侧 delta 上冲
         * ~150-250 (CSV 实测梯度), 200 与耦合噪声地板贴太近 → 提到 300 */
        int thr = (i > TOUCH_TOP_CH_COUNT) ? 300 : 500;
        bool active = (delta > thr);
        if (active) any_active = true;

        if (active && !s_ts.touched[i]) {
            s_ts.touch_start_tick[i] = now;
        } else if (!active) {
            s_ts.touch_start_tick[i] = 0;
        }
        s_ts.touched[i] = active;
        s_ts.filtered[i] = (s_ts.filtered[i] * 3 + (delta > 0 ? delta : 0)) / 4;
    }

    /* 动态基线: 全通道无触摸 → 基线向 raw 慢速追踪 (1/32 EMA)。
     * 拔电实测: USB 断开后供电轨变化 → 触摸 raw 持续偏移超固定
     * 阈值 250 → 每次息屏后 2-4s 必自动亮屏。基线固定不更新 = 永不追平。
     * 追踪条件与效果:
     * - 触摸时 (any_active) 不追 — 手指按下 delta 大, 追会把判定吃掉
     * - 环境态每轮收敛 delta 的 1/32: 50Hz 下 2s 追平 ~96% 偏移,
     * 10Hz 息屏兜底下 8s 追平 ~96% — 自动亮屏最多一次, 此后不复发
     * - 真实触摸 delta 突变 (>500) 远超单轮收敛量 (~15), 判定不受影响 */
    if (!any_active) {
        for (int i = 0; i < TOUCH_CH_COUNT; i++) {
            int32_t d = (int32_t)s_ts.raw[i] - (int32_t)s_ts.baseline[i];
            s_ts.baseline[i] += (uint32_t)(d >> 5);
        }
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

/* ── 息屏期唤醒探针 ( 简化) ──
 * 10Hz 兜底扫描的唤醒判定。曾维护息屏专用动态基线 (s_sleep_base
 * 无门控 1/4 EMA + settling 窗 + 跳变快进) — 用户拍板 (2026-08-23):
 * 睡眠侧不搞动态基线, 唤醒调优归触摸模块统一负责 (揉搓难的数据指向
 * 触摸模块 10Hz 扫描下 1/32 基线追踪把缓慢按压吃掉, 属模块基线策略,
 * 见 touch_scan_once)。本版直接复用触摸模块共享判定 s_ts.touched
 * (raw vs s_ts.baseline, any_active 门禁 1/32 EMA), 与亮屏触摸同一套
 * 数据 — 探针只剩去抖 + 空间上限:
 * - 连续 3 次 (300ms) 命中才唤醒 — 防瞬时噪声
 * - 超阈值通道 >10 判环境态 (全手掌覆盖 ≤10)
 * USB/供电瞬态由免疫窗覆盖 (拔插事件 → 5s 内不判唤醒, 数据:
 * 劈里啪啦/停充偏移在触摸 raw 上波动 ±100, 远低于 300/500 阈值) */
static uint8_t s_probe_hits = 0;
static TickType_t s_immune_until = 0; /* : USB/供电事件免疫窗 */
#define PROBE_HIT_REQUIRED 3
#define PROBE_MAX_CHANNELS 10

bool touch_fpc_sleep_probe(void)
{
    touch_scan_once();

    /* 免疫窗: USB 拔插/模式切换后 5s 内不判唤醒 — 瞬态 (VBUS
     * 消失/恢复, PHY 电源域切换) 使 raw 持续跳变 1-2s (实测假唤醒)。
     * 手动唤醒路径独立, 不在此窗内 */
    if (xTaskGetTickCount() < s_immune_until)
        return false;

    int n_touched = 0;
    for (int i = 0; i < TOUCH_CH_COUNT; i++)
        if (s_ts.touched[i]) n_touched++;

    if (n_touched == 0 || n_touched > PROBE_MAX_CHANNELS) {
        s_probe_hits = 0;
        return false;
    }
    if (++s_probe_hits < PROBE_HIT_REQUIRED) return false;
    s_probe_hits = 0;
    return true;
}

/* : USB/供电事件 → 5s 免疫窗 (见 touch_fpc.h 注释) */
void touch_fpc_note_usb_event(void)
{
    s_immune_until = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
    s_probe_hits = 0;
}

/* 诊断 : 去抖命中数 — power_log.csv ph 列 */
uint8_t touch_fpc_probe_hits(void) { return s_probe_hits; }

/* 诊断 : 当前超阈值通道数 — power_log.csv tc 列 */
uint8_t touch_fpc_touched_count(void)
{
    uint8_t n = 0;
    for (int i = 0; i < TOUCH_CH_COUNT; i++)
        if (s_ts.touched[i]) n++;
    return n;
}

/* ── 周期性扫描定时器回调 (50 Hz) ── */
static void touch_scan_timer_cb(TimerHandle_t timer)
{
    touch_scan_once();
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

    /* 创建周期性扫描定时器（50 Hz） */
    s_scan_timer = xTimerCreate(
        "touch_scan",
        pdMS_TO_TICKS(TOUCH_SAMPLE_PERIOD_MS),
        pdTRUE, /* 自动重载 */
        NULL,
        touch_scan_timer_cb);
    if (!s_scan_timer) {
        ESP_LOGE(TAG, "创建扫描定时器失败");
        return ESP_ERR_NO_MEM;
    }
    xTimerStart(s_scan_timer, 0);

    ESP_LOGI(TAG, "触摸 FPC 已初始化（12 通道，%d Hz）",
             (int)(1000 / TOUCH_SAMPLE_PERIOD_MS));
    return ESP_OK;
}

void touch_fpc_scan(void)
{
    /* 空操作：扫描在定时器回调中完成。
     * 此函数保留以保持 API 兼容性。 */
}

void touch_fpc_poll_once(void)
{
    /* : 主循环 1s 块调用的兜底单发扫描 — 息屏期 50Hz 定时器已停
     * (touch_fpc_pause, 停着才有 ≥1s 睡眠窗口), 硬件触摸唤醒依赖轻睡
     * 真正进入 (esp_sleep_enable_touchpad_wakeup 只在睡眠中触发) —
     * 轻睡不生效时 (如 USB 插着时, 1.0.232 实测"叫不醒"困死在 U盘
     * 模式) 触摸就没有任何消费者。1Hz 直读保持 filtered/touched 新鲜,
     * 主循环的 contacted 检查 (filtered>250) 就能唤醒 — 无论轻睡是否
     * 进入, 触摸永远有人管。成本: 12 次 raw 读/秒, 可忽略。 */
    touch_scan_once();
}

/* ── 睡眠节电 ── */

void touch_fpc_prepare_wakeup(void)
{
    for (int i = 0; i < TOUCH_CH_COUNT; i++) {
        /* S3 硬件唤醒比较器: raw − 硬件基准 > 阈值 触发。基准在首轮测量
         * 自初始化 (≈ 软件基线)。阈值 = 软件检测阈值原值 (顶部 delta>500 /
         * 右侧 delta>200) — 1.0.232 曾折半取阈 (200/120) 想唤醒轻触, 结果
         * 低于环境噪声地板 (桌面环境滤波值常驻 100~190, 代码注释"不算
         * 操作"): 环境瞬态 (拔线振动/桌面震动) 触发假唤醒 + 幻触点击 UI
         * (实测: 自动唤醒后点开设置页 U盘开关, 设备困死在 U盘模式)。
         * 阈值 = 软件判定一致 ⇒ 唤醒的触摸 = 软件也会判为触摸的触摸,
         * 环境永远差一截。轻触 (初按 delta~150) 不唤醒 — 接受: 唤醒后
         * 手指加深按下即可, 可靠性优先于极致灵敏度。 */
        uint32_t wthr = (i > TOUCH_TOP_CH_COUNT) ? 200 : 500;
        touch_pad_set_thresh(s_pads[i], wthr);
        uint32_t bench = 0;
        touch_pad_read_benchmark(s_pads[i], &bench);
        ESP_LOGI(TAG, "CH%d 唤醒阈值=%lu 硬件基准=%lu 软件基线=%lu",
                 i, (unsigned long)wthr, (unsigned long)bench,
                 (unsigned long)s_ts.baseline[i]);
    }
}

void touch_fpc_pause(void)
{
    if (s_scan_timer) xTimerStop(s_scan_timer, 0);
    s_probe_hits = 0; /* : 息屏专用基线已删 — 唤醒判定走共享基线 */
}

void touch_fpc_resume(void)
{
    /* 亮屏唤醒 = 刷新基线基准: 睡眠期供电/耦合漂移会让软件判定
     * (raw vs 基线) 失真 — 重校准后首轮扫描即新基线, 判定恢复。
     * 校准 500ms 期间先停扫描定时器 (防并发读写基线), 校准完再
     * 恢复扫描。校准内置 raw 全 0 保护: 全 0 时不覆盖基线, 不动作 */
    if (s_scan_timer) xTimerStop(s_scan_timer, 0);
    s_probe_hits = 0;
    touch_auto_calibrate();
    if (s_scan_timer) xTimerStart(s_scan_timer, 0);
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
void touch_get_baseline(uint32_t *out) { memcpy(out, s_ts.baseline, sizeof(s_ts.baseline)); }
void touch_get_filtered(int *out) { memcpy(out, s_ts.filtered, sizeof(s_ts.filtered)); }
