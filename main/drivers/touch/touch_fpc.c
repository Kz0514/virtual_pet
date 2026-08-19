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
    TOUCH_PAD_NUM2,   /* 0: GPIO2  — 左侧 */
    TOUCH_PAD_NUM3,   /* 1: GPIO3  — 顶部 0 */
    TOUCH_PAD_NUM4,   /* 2: GPIO4  — 顶部 1 */
    TOUCH_PAD_NUM5,   /* 3: GPIO5  — 顶部 2 */
    TOUCH_PAD_NUM6,   /* 4: GPIO6  — 顶部 3 */
    TOUCH_PAD_NUM7,   /* 5: GPIO7  — 顶部 4 */
    TOUCH_PAD_NUM8,   /* 6: GPIO8  — 右侧 0 */
    TOUCH_PAD_NUM9,   /* 7: GPIO9  — 右侧 1 */
    TOUCH_PAD_NUM10,  /* 8: GPIO10 — 右侧 2 */
    TOUCH_PAD_NUM11,  /* 9: GPIO11 — 右侧 3 */
    TOUCH_PAD_NUM12,  /* 10: GPIO12 — 右侧 4 */
    TOUCH_PAD_NUM13,  /* 11: GPIO13 — 右侧 5 */
};

/* 状态 */
typedef struct {
    uint32_t baseline[TOUCH_CH_COUNT];
    uint32_t raw[TOUCH_CH_COUNT];
    bool     touched[TOUCH_CH_COUNT];
    int      filtered[TOUCH_CH_COUNT];
    uint32_t touch_start_tick[TOUCH_CH_COUNT];
    float    smooth_left;
    float    smooth_top_pos;
    float    smooth_right_pos;
    float    raw_top_pos;      /* 未平滑顶部质心 — 滑动检测用(平滑系数会低估位移) */
} touch_state_t;

static touch_state_t s_ts = {0};
static TimerHandle_t  s_scan_timer = NULL;

/* ── 自动校准 ── */
static void touch_auto_calibrate(void)
{
    ESP_LOGI(TAG, "正在自动校准触摸基线…");
    uint64_t sum[TOUCH_CH_COUNT] = {0};

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
        ESP_LOGI(TAG, "  CH%d 基线=%lu", i, s_ts.baseline[i]);
    }
    ESP_LOGI(TAG, "校准完成");
}

/* ── 周期性扫描定时器回调 ── */
static void touch_scan_timer_cb(TimerHandle_t timer)
{
    uint32_t now = xTaskGetTickCount();

    for (int i = 0; i < TOUCH_CH_COUNT; i++) {
        touch_pad_read_raw_data(s_pads[i], &s_ts.raw[i]);

        /* ESP32-S3: 触摸时 raw 值上升 (delta = raw - baseline > 0) */
        int32_t delta = (int32_t)s_ts.raw[i] - (int32_t)s_ts.baseline[i];
        /* Right-side channels (6-11) have smaller electrodes → lower threshold */
        int thr = (i > TOUCH_TOP_CH_COUNT) ? 200 : 500;
        bool active = (delta > thr);

        if (active && !s_ts.touched[i]) {
            s_ts.touch_start_tick[i] = now;
        } else if (!active) {
            s_ts.touch_start_tick[i] = 0;
        }
        s_ts.touched[i] = active;
        s_ts.filtered[i] = (s_ts.filtered[i] * 3 + (delta > 0 ? delta : 0)) / 4;
    }

    /* 顶部滑块（通道 1-5）：加权质心 → -1..+1 */
    float t_w = 0, t_s = 0;
    for (int i = 1; i <= TOUCH_TOP_CH_COUNT; i++) {
        if (s_ts.touched[i]) { float w = (float)s_ts.filtered[i]; t_w += w * (i - 1); t_s += w; }
    }
    float tp = (t_s > 0) ? (t_w / t_s) / (TOUCH_TOP_CH_COUNT - 1) : -1.0f;
    tp = tp * 2.0f - 1.0f;
    s_ts.raw_top_pos = tp;
    s_ts.smooth_top_pos = s_ts.smooth_top_pos * 0.7f + tp * 0.3f;

    /* 右侧滑块（通道 6-11）：加权质心 → -1..+1 */
    float r_w = 0, r_s = 0;
    for (int i = TOUCH_TOP_CH_COUNT + 1; i < TOUCH_CH_COUNT; i++) {
        if (s_ts.touched[i]) { float w = (float)s_ts.filtered[i]; r_w += w * (i - TOUCH_TOP_CH_COUNT - 1); r_s += w; }
    }
    float rp = (r_s > 0) ? (r_w / r_s) / (TOUCH_RIGHT_CH_COUNT - 1) : -1.0f;
    rp = rp * 2.0f - 1.0f;
    s_ts.smooth_right_pos = s_ts.smooth_right_pos * 0.3f + rp * 0.7f;  /* faster response */

    /* 左侧按钮（通道 0） */
    s_ts.smooth_left = s_ts.smooth_left * 0.7f + (s_ts.touched[0] ? 1.0f : 0.0f) * 0.3f;
}

/* ── 公开 API ── */
esp_err_t touch_fpc_init(void)
{
    ESP_LOGI(TAG, "正在初始化 12 通道 FPC 触摸传感器（旧版 API）…");

    /* 初始化触摸外设: 硬件定时器自动触发测量 */
    touch_pad_init();
    touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);  /* 硬件自动触发 */
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
        pdTRUE,   /* 自动重载 */
        NULL,
        touch_scan_timer_cb
    );
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

bool touch_fpc_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    lv_coord_t x = (lv_coord_t)((s_ts.smooth_top_pos + 1.0f) / 2.0f * DISPLAY_WIDTH);
    lv_coord_t y = (lv_coord_t)((s_ts.smooth_right_pos + 1.0f) / 2.0f * DISPLAY_HEIGHT);

    data->point.x = x;
    data->point.y = y;
    data->state   = (s_ts.touched[0] || (fabsf(s_ts.smooth_top_pos) > 0.1f)
                      || (fabsf(s_ts.smooth_right_pos) > 0.1f))
                    ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    return false;
}

/* ── 公开查询接口 ── */
bool     touch_is_left_pressed(void)  { return s_ts.touched[0]; }
uint32_t touch_left_hold_ms(void)     { return s_ts.touched[0] ? (xTaskGetTickCount() - s_ts.touch_start_tick[0]) * portTICK_PERIOD_MS : 0; }
float    touch_top_position(void)     { return s_ts.smooth_top_pos; }
float    touch_top_position_raw(void) { return s_ts.raw_top_pos; }
float    touch_right_position(void)   { return s_ts.smooth_right_pos; }

bool touch_is_top_pressed(void) {
    /* 顶部滑条 5 通道 (索引 1-5) 任一被触摸 */
    for (int i = 1; i <= TOUCH_TOP_CH_COUNT; i++) {
        if (s_ts.touched[i]) return true;
    }
    return false;
}

bool touch_is_right_pressed(void) {
    /* Check if any right-side channel (6-11) is actually touched */
    for (int i = TOUCH_TOP_CH_COUNT + 1; i < TOUCH_CH_COUNT; i++) {
        if (s_ts.touched[i]) return true;
    }
    return false;
}

bool touch_is_petting_head(void)
{
    int n = 0;
    for (int i = 1; i <= TOUCH_TOP_CH_COUNT; i++) { if (s_ts.touched[i]) n++; }
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
