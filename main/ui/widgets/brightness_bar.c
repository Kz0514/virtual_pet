/**
 * @file brightness_bar.c
 * @brief 亮度调节 — lv_bar 垂直条 + label, 10ms 轮询
 */
#include "brightness_bar.h"
#include "touch_fpc.h"
#include "st7789.h"
#include "tm6604.h"
#include "tap_detector.h"
#include "shake_detector.h"
#include "settings_screen.h"
#include "config_mgr.h"
#include "lvgl.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static const char *TAG = "bri_bar";

/* NVS 配置键 (config_mgr, ns "settings") */
#define CFG_KEY_BRI     "bri"        /* 用户亮度 1-100 */
#define CFG_KEY_BAR_EN  "bri_bar_en" /* 主界面亮度条开关 */

static lv_obj_t   *s_bar   = NULL;
static lv_obj_t   *s_label = NULL;
static lv_timer_t *s_poll  = NULL;
static lv_timer_t *s_hide  = NULL;

static uint8_t s_bri = 80;  /* 用户当前设定亮度 (副本, 供其他任务安全读取) */
static uint8_t s_last_bri  = 80;   /* 上次应用的亮度 (振动反馈节流/变化检测) */
static uint32_t s_last_vib = 0;    /* 上次振动时间 (100ms 节流) */
static bool s_was_busy = false;    /* 抑制状态缓存 (调节期间抑制摇动/敲击检测) */
static bool s_enabled  = true;     /* 主界面亮度条开关 (设置页可调) */

static uint32_t s_press_start;   /* 中间两触点按下起始时间 */
static bool s_active;            /* 正在调节中 */
static bool s_unlocked;          /* 解锁窗口 (中间两点长按后5秒内可调) */
static uint32_t s_unlock_expire;

#define POLL_MS     10
#define LONG_PRESS  500
#define UNLOCK_MS   5000   /* 解锁后调节窗口 */
#define HIDE_DELAY  1000
#define BAR_H       150
#define BAR_W       18

/* 右侧滑条中间触点 (通道8或9) 是否按下 */
static bool two_middle_pressed(void) {
    int f[12];
    touch_get_filtered(f);
    return (f[8] > 100 || f[9] > 100);
}

static void hide_cb(lv_timer_t *t) {
    if (s_bar)   lv_obj_add_flag(s_bar,   LV_OBJ_FLAG_HIDDEN);
    if (s_label) lv_obj_add_flag(s_label, LV_OBJ_FLAG_HIDDEN);
    s_hide = NULL;
}

/* 强制回到空闲态 — 页面切换/禁用时调用。
 * 修复: busy 期间切入设置页会让 poll 早退, tap/shake 抑制残留。 */
static void force_idle(void) {
    if (s_was_busy) {
        s_was_busy = false;
        tap_detector_suppress(false);
        shake_detector_suppress(false);
    }
    s_unlocked = false;
    s_active = false;
    s_press_start = 0;
    if (s_hide) { lv_timer_delete(s_hide); s_hide = NULL; }
    if (s_bar)   lv_obj_add_flag(s_bar,   LV_OBJ_FLAG_HIDDEN);
    if (s_label) lv_obj_add_flag(s_label, LV_OBJ_FLAG_HIDDEN);
}

static void poll_cb(lv_timer_t *t) {
    /* 禁用或设置界面激活: 右侧滑条归菜单导航, 清状态后跳过调节 */
    if (!s_enabled || settings_screen_is_active()) {
        force_idle();
        return;
    }

    /* ── 解锁: 长按中间两触点 ── */
    if (two_middle_pressed()) {
        if (s_press_start == 0) s_press_start = xTaskGetTickCount();
        if (xTaskGetTickCount() - s_press_start >= pdMS_TO_TICKS(LONG_PRESS)) {
            if (!s_unlocked) ESP_LOGI(TAG, "中间两触点长按 — 亮度调节解锁");
            s_unlocked = true;
            s_unlock_expire = xTaskGetTickCount() + pdMS_TO_TICKS(UNLOCK_MS);
        }
    } else {
        s_press_start = 0;
        if (s_unlocked && xTaskGetTickCount() > s_unlock_expire)
            s_unlocked = false;   /* 窗口过期 */
    }

    /* ── 调节: 解锁窗口内用右侧滑块 ── */
    bool right = touch_is_right_pressed();
    if (s_unlocked && right) {
        s_active = true;

        lv_obj_clear_flag(s_bar,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_label, LV_OBJ_FLAG_HIDDEN);
        if (s_hide) { lv_timer_delete(s_hide); s_hide = NULL; }

        float pos = touch_right_position();
        int bri = (int)((1.0f - pos) / 2.0f * 100.0f);
        if (bri < 1)  bri = 1;
        if (bri > 100) bri = 100;

        if (bri != s_last_bri) {
            s_last_bri = (uint8_t)bri;
            /* 轻振反馈, 100ms 节流 (TM6604 低占空比不起振, 60ms 保证机械起振) */
            if (xTaskGetTickCount() - s_last_vib >= pdMS_TO_TICKS(100)) {
                s_last_vib = xTaskGetTickCount();
                tm6604_vibrate(55, 60);
            }
        }

        lv_bar_set_value(s_bar, bri, LV_ANIM_OFF);
        s_bri = (uint8_t)bri;
        st7789_backlight_set(bri);

        char buf[8];
        snprintf(buf, sizeof(buf), "%hhu%%", (uint8_t)bri);
        lv_label_set_text(s_label, buf);
    } else {
        if (s_active) {
            s_active = false;
            /* 松手落盘 — 不在拖动过程中写 NVS (频率过高) */
            config_set_u32(CFG_KEY_BRI, s_bri);
            if (s_hide) lv_timer_delete(s_hide);
            s_hide = lv_timer_create(hide_cb, HIDE_DELAY, NULL);
            lv_timer_set_repeat_count(s_hide, 1);
        }
    }

    /* 调节期间 (解锁窗口/滑动中) 抑制摇动与敲击检测 —
     * 手指滑动 + 振动反馈会被 20Hz 检测器误判成事件 */
    bool busy = (s_unlocked || s_active);
    if (busy != s_was_busy) {
        s_was_busy = busy;
        tap_detector_suppress(busy);
        shake_detector_suppress(busy);
    }
}

void brightness_bar_init(void) {
    /* 载入持久化配置: 亮度 + 亮度条开关 */
    s_bri = (uint8_t)config_get_u32(CFG_KEY_BRI, 80);
    if (s_bri < 1) s_bri = 1;
    if (s_bri > 100) s_bri = 100;
    s_last_bri  = s_bri;
    s_enabled   = config_get_u32(CFG_KEY_BAR_EN, 1) != 0;
    st7789_backlight_set(s_bri);   /* 用持久化亮度覆盖开机默认 80 */

    /* Bar: bare vertical, no container frame */
    s_bar = lv_bar_create(lv_screen_active());
    lv_obj_set_size(s_bar, BAR_W, BAR_H);
    lv_obj_set_pos(s_bar, 214, 45);
    lv_bar_set_range(s_bar, 1, 100);
    lv_bar_set_value(s_bar, s_bri, LV_ANIM_OFF);
    lv_bar_set_orientation(s_bar, LV_BAR_ORIENTATION_VERTICAL);
    lv_bar_set_mode(s_bar, LV_BAR_MODE_NORMAL);
    lv_obj_set_style_radius(s_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 4, LV_PART_INDICATOR);  /* same radius */
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x222244), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);

    /* Label below */
    s_label = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_color(s_label, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(s_label, &lv_font_montserrat_14, 0);
    char init_txt[8];
    snprintf(init_txt, sizeof(init_txt), "%hhu%%", s_bri);
    lv_label_set_text(s_label, init_txt);
    lv_obj_align_to(s_label, s_bar, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
    lv_obj_add_flag(s_label, LV_OBJ_FLAG_HIDDEN);

    /* 修复 "100% 时屏幕底部灰色横条":
     * "100%" 是唯一 4 字符的标签文本, 居中对齐后比 3 字符值宽出 ~9px,
     * 右缘越过屏幕右边界 ~2px → LVGL 判定屏幕内容可滚动 →
     * 主题滚动条样式 (灰色 / RADIUS_CIRCLE 半圆端 / OPA_40) 自动出现在屏幕底部,
     * 且随本控件隐藏而消失。主屏幕无任何滚动交互 (无 indev), 关闭滚动条显示。 */
    lv_obj_set_scrollbar_mode(lv_screen_active(), LV_SCROLLBAR_MODE_OFF);

    s_poll = lv_timer_create(poll_cb, POLL_MS, NULL);
    lv_timer_set_repeat_count(s_poll, -1);

    ESP_LOGI(TAG, "亮度条就绪 (%dHz, 亮度 %hhu%%, %s)",
             1000 / POLL_MS, s_bri, s_enabled ? "启用" : "禁用");
}

uint8_t brightness_bar_get(void) { return s_bri; }

void brightness_bar_set(uint8_t bri) {
    if (bri < 1) bri = 1;
    if (bri > 100) bri = 100;
    s_bri = bri;
    s_last_bri = bri;
    st7789_backlight_set(bri);
    /* 同步 UI (若条正在显示) */
    if (s_bar) lv_bar_set_value(s_bar, bri, LV_ANIM_OFF);
    if (s_label) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%hhu%%", bri);
        lv_label_set_text(s_label, buf);
    }
}

void brightness_bar_set_enabled(bool en) {
    if (s_enabled == en) return;
    s_enabled = en;
    ESP_LOGI(TAG, "亮度条%s", en ? "启用" : "禁用");
    if (!en) force_idle();
}
