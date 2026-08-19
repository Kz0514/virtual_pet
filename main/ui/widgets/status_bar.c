/**
 * @file status_bar.c
 * @brief 顶部状态栏 — WiFi + 电池, FontAwesome 图标
 */
#include "status_bar.h"
#include "esp_log.h"
#include "lvgl.h"
#include "src/font/lv_symbol_def.h"
#include <stdio.h>

static const char *TAG = "status_bar";

static lv_obj_t *s_wifi_label;
static lv_obj_t *s_bat_label;
static lv_timer_t *s_timer;

static volatile bool    s_wifi_connected;
static volatile uint8_t s_bat_pct;
static volatile bool    s_dirty = true;

static void update_timer_cb(lv_timer_t *t) {
    if (!s_dirty) return;
    s_dirty = false;

    lv_label_set_text(s_wifi_label, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(s_wifi_label,
        s_wifi_connected ? lv_color_hex(0xFFFFFF) : lv_color_hex(0xFF4444), 0);

    const char *baticon = LV_SYMBOL_BATTERY_FULL;
    if      (s_bat_pct < 10) baticon = LV_SYMBOL_BATTERY_EMPTY;
    else if (s_bat_pct < 35) baticon = LV_SYMBOL_BATTERY_1;
    else if (s_bat_pct < 60) baticon = LV_SYMBOL_BATTERY_2;
    else if (s_bat_pct < 85) baticon = LV_SYMBOL_BATTERY_3;
    lv_label_set_text(s_bat_label, baticon);
}

void status_bar_init(void) {
    /* 无背景容器 — 图标直接放在屏幕右上角 */

    /* WiFi — 距右 30px, 垂直居中于 24px 状态栏高度 */
    s_wifi_label = lv_label_create(lv_screen_active());
    lv_label_set_text(s_wifi_label, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(s_wifi_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_wifi_label, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_wifi_label, 240 - 30 - 14, 5);

    /* Battery — 距右 6px, 垂直居中于 24px 状态栏高度 */
    s_bat_label = lv_label_create(lv_screen_active());
    lv_label_set_text(s_bat_label, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_color(s_bat_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_bat_label, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(s_bat_label, 240 - 6 - 16, 4);

    s_timer = lv_timer_create(update_timer_cb, 1000, NULL);
    lv_timer_set_repeat_count(s_timer, -1);

    ESP_LOGI(TAG, "状态栏就绪");
}

void status_bar_set_wifi(bool connected, int8_t rssi) {
    s_wifi_connected = connected;
    s_dirty = true;
}

void status_bar_set_battery(uint8_t pct, uint16_t voltage_mv) {
    s_bat_pct = pct;
    s_dirty = true;
}
