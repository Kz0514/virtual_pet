/** @file input_handler.c @brief 页面交互仲裁器
 *
 * 职责:
 *  1. 接管 gesture_detect 事件回调 + 20ms 手势定时器
 *  2. 维护当前页面 (HOME / SETTINGS), 页面特性表:
 *       HOME:     摇动/敲击/摸头/长按语音/三击静音 全开
 *       SETTINGS: 上述全关 (检测器排空, 手势不路由)
 *  3. 进出页钩子: menu_active 语义、跨页拼击防护 (gesture_reset_taps)、
 *     摸头电极与导航电极物理重叠的禁用、亮度条 busy 遗留抑制的清除
 *
 * 路由:
 *   通用:  WAKE → 唤醒; OPEN_MENU(HOME) → 进设置页
 *   HOME:  PETTING / VOICE / MUTE → home_interaction
 *   SETTINGS (按 NVS "nav_mode" 选择逻辑):
 *     左键单击 = CONFIRM (恒成立); 顶条左/右滑 = CONFIRM/BACK (恒成立)
 *     点击模式: 右滑条轻点上/下半段 = UP/DOWN; 顶条轻点左/右半区 = BACK/CONFIRM
 *     滑动模式: 右滑条上/下滑动 = UP/DOWN
 *   CLOSE_MENU 在设置页无动作 (双击不退出, 防与单击确认误触)
 */
#include "input_handler.h"
#include "home_interaction.h"
#include "home_screen.h"
#include "gesture_detect.h"
#include "settings_screen.h"
#include "diary_screen.h"
#include "config_mgr.h"
#include "config_keys.h"
#include "pat_detector.h"
#include "tap_detector.h"
#include "shake_detector.h"
#include "power_manager.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "input";

static app_page_t s_page = APP_PAGE_HOME;

/* 打开冷却: 双击开门后 300ms 内不路由设置输入 —
 * 防开门动作的尾击 (菜单模式单击零延迟) 被当成 CONFIRM 吃掉 */
#define SETTINGS_OPEN_COOLDOWN_MS 300
static uint32_t s_settings_open_tick = 0;

/* 选择逻辑模式 (NVS "nav_mode", 设置页可调):
 * 0 = 点击: 右滑条轻点上/下半段选择; 顶条轻点左半区=返回/右半区=确认
 * (顶条左右滑动作为超集保留)
 * 1 = 滑动: 右滑条上/下滑动选择; 顶条左滑=确认/右滑=返回
 * 左键单击=确认 恒成立。每事件读取 (config_mgr 有 RAM 缓存, 开销可忽略)。
 * 默认 1 (滑动)。 */
static bool nav_mode_is_tap(void)
{
    return config_get_u32(CFG_KEY_NAV_MODE, 1) == 0;
}

/* ── 设置页"退出"回调 (根页 BACK / 顶条右滑到底时触发) ── */
static void on_settings_close(void)
{
    settings_screen_destroy();
    home_screen_restore();
    input_handler_set_page(APP_PAGE_HOME);
}

/* ── 页面特性表: 进/出页钩子 ── */
void input_handler_set_page(app_page_t page)
{
    if (page == s_page) return;
    s_page = page;

    if (page == APP_PAGE_SETTINGS) {
        gesture_set_menu_active(true);   /* 左键单击=确认(零延迟), 双击退出取消 */
        gesture_reset_taps();            /* 开门的双击不得拼进设置页的按键序列 */
        pat_detector_set_enabled(false); /* 导航电极与摸头电极物理重叠 */
        tap_detector_suppress(false);    /* 清亮度条 busy 可能遗留的抑制 */
        shake_detector_suppress(false);
        home_interaction_set_enabled(false); /* 摇动/敲击只排空不反应 */
    } else {
        gesture_set_menu_active(false);
        gesture_reset_taps(); /* 回主页同样防跨页拼击 */
        pat_detector_set_enabled(true);
        home_interaction_set_enabled(true);
    }
    ESP_LOGI(TAG, "页面 → %s", page == APP_PAGE_SETTINGS ? "SETTINGS" : "HOME");
}

app_page_t input_handler_get_page(void)
{
    /* 对账: 设置页可能自行销毁而未通知本模块 */
    if (s_page == APP_PAGE_SETTINGS && !settings_screen_is_active()) {
        input_handler_set_page(APP_PAGE_HOME);
    }
    return s_page;
}

/* ── 设置路由: 日记页打开时事件优先注入日记, 否则设置页 ──
 * 日记是挂在设置页上的独立 screen (设置页保持存活), BACK 到列表根
 * 再按一次即 destroy 自身并切回设置页, 此后自动回落设置路由。 */
static void route_settings(menu_event_t sev)
{
    if (diary_screen_is_active())
        diary_screen_input(sev);
    else
        settings_screen_input(sev);
}

/* ── 手势事件路由 (LVGL 定时器上下文) ── */
static void on_gesture_event(gesture_event_t ev)
{
    app_page_t page = input_handler_get_page(); /* 先对账 */

    switch (ev) {
    case GESTURE_WAKE_SCREEN:
        power_manager_note_interaction();
        break;

    case GESTURE_OPEN_MENU:
        if (page == APP_PAGE_HOME && !settings_screen_is_active()) {
            power_manager_note_interaction();
            input_handler_set_page(APP_PAGE_SETTINGS);
            settings_screen_set_close_cb(on_settings_close);
            settings_screen_init();
            s_settings_open_tick = lv_tick_get();
        }
        break;

    case GESTURE_CLOSE_MENU:
        /* 双击在设置页无动作 — 退出统一走顶条右滑 (防与单击确认误触) */
        break;

    case GESTURE_VOICE_TRIGGER:
    case GESTURE_MUTE_TOGGLE:
        if (page == APP_PAGE_HOME) {
            home_interaction_on_gesture(ev);
        }
        break;

    case GESTURE_SINGLE_TAP:
    case GESTURE_NAV_UP:
    case GESTURE_NAV_DOWN:
    case GESTURE_NAV_SLIDE_UP:
    case GESTURE_NAV_SLIDE_DOWN:
    case GESTURE_TOP_TAP_LEFT:
    case GESTURE_TOP_TAP_RIGHT:
    case GESTURE_SWIPE_LEFT:
    case GESTURE_SWIPE_RIGHT:
        if (page == APP_PAGE_SETTINGS && settings_screen_is_active()) {
            /* 打开冷却: 吞掉开门双击可能遗留的尾击 */
            if ((uint32_t)(lv_tick_get() - s_settings_open_tick) < SETTINGS_OPEN_COOLDOWN_MS) {
                break;
            }
            power_manager_note_interaction(); /* 操作中保持常亮 */
            bool tap = nav_mode_is_tap();
            switch (ev) {
            case GESTURE_SINGLE_TAP: /* 左键确认恒成立 */
                route_settings(MENU_EV_CONFIRM);
                break;
            case GESTURE_NAV_UP: /* 点击模式: 选择 */
                if (tap) route_settings(MENU_EV_UP);
                break;
            case GESTURE_NAV_DOWN:
                if (tap) route_settings(MENU_EV_DOWN);
                break;
            case GESTURE_NAV_SLIDE_UP: /* 滑动模式: 选择 */
                if (!tap) route_settings(MENU_EV_UP);
                break;
            case GESTURE_NAV_SLIDE_DOWN:
                if (!tap) route_settings(MENU_EV_DOWN);
                break;
            case GESTURE_TOP_TAP_LEFT: /* 点击模式: 返回 */
                if (tap) route_settings(MENU_EV_BACK);
                break;
            case GESTURE_TOP_TAP_RIGHT: /* 点击模式: 确认 */
                if (tap) route_settings(MENU_EV_CONFIRM);
                break;
            case GESTURE_SWIPE_RIGHT: /* 两模式均可: 返回 */
                route_settings(MENU_EV_BACK);
                break;
            case GESTURE_SWIPE_LEFT: /* 两模式均可: 确认 */
                route_settings(MENU_EV_CONFIRM);
                break;
            default:
                break;
            }
        }
        break;

    default:
        break;
    }
}

/* ── 手势驱动: 20ms LVGL 定时器 (50Hz) ── */
static void gesture_timer_cb(lv_timer_t *t)
{
    (void)t;
    gesture_process();
}

esp_err_t input_handler_init(void)
{
    s_page = APP_PAGE_HOME;
    gesture_set_event_handler(on_gesture_event);
    lv_timer_t *tmr = lv_timer_create(gesture_timer_cb, 20, NULL);
    if (tmr) lv_timer_set_repeat_count(tmr, -1);
    home_interaction_set_enabled(true);
    ESP_LOGI(TAG, "页面交互仲裁就绪 (20ms 手势驱动)");
    return ESP_OK;
}
