/**
 * @file home_screen.c
 * @brief 主屏幕 — 线程安全设计: 数据存入全局变量, lv_timer在LVGL线程内读取并刷新UI
 */
#include "home_screen.h"
#include "font_loader.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "home";

/* 全局数据 (app_main写入, lv_timer回调读取) */
static float    s_temp=0, s_hum=0, s_lux=0;
static uint16_t s_bat_mv=0, s_bat_pct=0;
static float    s_bat_temp=0;
static int16_t  s_current_ma=0;
static uint16_t s_remain_mah=0, s_full_mah=0;
static bool     s_wifi_ok = false;
static char     s_wifi_ip[32] = "--";
static char     s_chat_full[512] = "";   // full reply
static int      s_chat_pos = 0;          // current display position
static bool     s_chat_active = false;

/* LVGL widgets */
static lv_obj_t *lbl_temp=NULL, *lbl_hum=NULL, *lbl_lux=NULL, *lbl_bat=NULL, *lbl_wifi=NULL;
static lv_obj_t *lbl_bat_temp=NULL, *lbl_current=NULL, *lbl_capacity=NULL;
static lv_obj_t *lbl_chat=NULL;

#define C_DARK  lv_color_hex(0x1a1a2e)
#define C_CARD  lv_color_hex(0x16213e)
#define C_RED   lv_color_hex(0xe94560)
#define C_GREEN lv_color_hex(0x4ecca3)
#define C_WHITE lv_color_hex(0xeeeeee)
#define C_DIM   lv_color_hex(0x888888)

/* ── 辅助 ── */
static lv_obj_t *make_label(lv_obj_t *p, const char *txt, lv_color_t c) {
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    return l;
}

/* ── LVGL 定时器: 每2秒从全局变量刷新UI ── */
static void update_timer_cb(lv_timer_t *timer)
{
    char buf[32];

    if (lbl_temp) { snprintf(buf, sizeof(buf), "%.1f C", s_temp); lv_label_set_text(lbl_temp, buf); }
    if (lbl_hum)  { snprintf(buf, sizeof(buf), "%.1f %%", s_hum); lv_label_set_text(lbl_hum, buf); }
    if (lbl_lux)  {
        if (s_lux >= 1000) snprintf(buf, sizeof(buf), "%.1f klx", s_lux/1000);
        else snprintf(buf, sizeof(buf), "%.0f lx", s_lux);
        lv_label_set_text(lbl_lux, buf);
    }
    if (lbl_bat)  {
        snprintf(buf, sizeof(buf), "%umV %u%%", s_bat_mv, s_bat_pct);
        lv_label_set_text(lbl_bat, buf);
        lv_obj_set_style_text_color(lbl_bat, s_bat_pct>20 ? C_GREEN : C_RED, 0);
    }
    if (lbl_wifi) {
        snprintf(buf, sizeof(buf), "%s", s_wifi_ok ? s_wifi_ip : "WiFi: off");
        lv_label_set_text(lbl_wifi, buf);
        lv_obj_set_style_text_color(lbl_wifi, s_wifi_ok ? C_GREEN : C_DIM, 0);
    }
    if (lbl_bat_temp) {
        snprintf(buf, sizeof(buf), "%.1f C", s_bat_temp);
        lv_label_set_text(lbl_bat_temp, buf);
    }
    if (lbl_current) {
        if (s_current_ma > 0)
            snprintf(buf, sizeof(buf), "+%dmA", s_current_ma);
        else if (s_current_ma < 0)
            snprintf(buf, sizeof(buf), "%dmA", s_current_ma);
        else
            snprintf(buf, sizeof(buf), "0mA");
        lv_label_set_text(lbl_current, buf);
        lv_obj_set_style_text_color(lbl_current,
            s_current_ma > 0 ? C_GREEN : (s_current_ma < 0 ? C_RED : C_WHITE), 0);
    }
    if (lbl_capacity) {
        snprintf(buf, sizeof(buf), "%u/%umAh", s_remain_mah, s_full_mah);
        lv_label_set_text(lbl_capacity, buf);
    }
}

/* ── 独立高速定时器: 逐字渲染聊天, |pXXX 暂停且不显示 ── */
static char s_display_buf[512];  /* filtered: no |pXXX markers */

static void chat_render_timer(lv_timer_t *timer)
{
    if (!lbl_chat || !s_chat_active) { lv_timer_del(timer); return; }

    /* Skip hidden pause marker |pXXX */
    if (strncmp(s_chat_full + s_chat_pos, "|p", 2) == 0) {
        int pause_ms = atoi(s_chat_full + s_chat_pos + 2);
        if (pause_ms > 0 && pause_ms < 5000) {
            s_chat_pos += 2;
            while (s_chat_full[s_chat_pos] >= '0' && s_chat_full[s_chat_pos] <= '9')
                s_chat_pos++;
            lv_timer_set_period(timer, pause_ms);
            return;  /* marker not added to display */
        }
    }

    /* Normal: copy next visible char to display buffer */
    lv_timer_set_period(timer, 50);
    int dlen = strlen(s_display_buf);
    s_display_buf[dlen] = s_chat_full[s_chat_pos];
    s_display_buf[dlen + 1] = '\0';
    s_chat_pos++;
    lv_label_set_text(lbl_chat, s_display_buf);

    if (s_chat_pos >= strlen(s_chat_full)) {
        s_chat_active = false;
        lv_timer_del(timer);
    }
}

/* ── 创建主屏幕 ── */
void home_screen_create(void)
{
    ESP_LOGI(TAG, "创建主屏幕…");

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, C_DARK, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* 标题栏 */
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, 240, 30);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(hdr, C_CARD, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);

    lv_obj_t *t = lv_label_create(hdr);
    lv_label_set_text(t, "Virtualpet");
    lv_obj_set_style_text_color(t, C_RED, 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 8, 0);

    lbl_wifi = lv_label_create(hdr);
    lv_label_set_text(lbl_wifi, "WiFi: --");
    lv_obj_set_style_text_color(lbl_wifi, C_DIM, 0);
    lv_obj_set_style_text_font(lbl_wifi, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_wifi, LV_ALIGN_RIGHT_MID, -8, 0);

    /* 传感器卡片 */
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, 224, 200);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_bg_color(card, C_CARD, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_radius(card, 12, 0);

    /* ── 第1行: 环境温度(左) + 湿度(右) ── */
    make_label(card, "Temp", C_DIM);
    lbl_temp = make_label(card, "--.- C", C_WHITE);
    lv_obj_align(lbl_temp, LV_ALIGN_TOP_LEFT, 8, 16);

    make_label(card, "Humidity", C_DIM);
    lbl_hum = make_label(card, "--.- %", C_WHITE);
    lv_obj_align(lbl_hum, LV_ALIGN_TOP_MID, 0, 16);

    /* ── 第2行: 电池温度(左) + 电流(右) ── */
    lv_obj_t *btt = make_label(card, "Bat Temp", C_DIM);
    lv_obj_align(btt, LV_ALIGN_TOP_LEFT, 8, 62);
    lbl_bat_temp = make_label(card, "--.- C", C_WHITE);
    lv_obj_align_to(lbl_bat_temp, btt, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);

    lv_obj_t *ct = make_label(card, "Current", C_DIM);
    lv_obj_align(ct, LV_ALIGN_TOP_MID, 0, 62);
    lbl_current = make_label(card, "---mA", C_WHITE);
    lv_obj_align_to(lbl_current, ct, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);

    /* ── 第3行: 电量(左) + 容量(右) ── */
    lv_obj_t *btitle = make_label(card, "Battery", C_DIM);
    lv_obj_align(btitle, LV_ALIGN_TOP_LEFT, 8, 108);
    lbl_bat = make_label(card, "----mV --%", C_GREEN);
    lv_obj_align_to(lbl_bat, btitle, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);

    lv_obj_t *capt = make_label(card, "Capacity", C_DIM);
    lv_obj_align(capt, LV_ALIGN_TOP_MID, 0, 108);
    lbl_capacity = make_label(card, "--/--mAh", C_WHITE);
    lv_obj_align_to(lbl_capacity, capt, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);

    /* 光照: 卡片右下角小字 */
    lv_obj_t *ltitle = make_label(card, "Light", C_DIM);
    lv_obj_align(ltitle, LV_ALIGN_BOTTOM_RIGHT, -8, -22);
    lbl_lux = make_label(card, "--.- lx", C_WHITE);
    lv_obj_align_to(lbl_lux, ltitle, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 2);

    /* 启动2秒刷新定时器 */
    lv_timer_create(update_timer_cb, 2000, NULL);

    /* 聊天回复区域 (底部) */
    lbl_chat = lv_label_create(scr);
    lv_label_set_long_mode(lbl_chat, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_chat, 220);
    lv_label_set_text(lbl_chat, "");
    lv_obj_set_style_text_color(lbl_chat, lv_color_hex(0xcccccc), 0);
    lv_obj_set_style_text_font(lbl_chat, FONT_ZH, 0);
    lv_obj_align(lbl_chat, LV_ALIGN_BOTTOM_MID, 0, -4);

    ESP_LOGI(TAG, "主屏幕就绪");
}

/* ── 线程安全: 仅写入全局变量 ── */
void home_screen_set_data(float temp, float humidity, float lux,
                           uint16_t battery_mv, uint16_t battery_pct,
                           float bat_temp_c, int16_t current_ma,
                           uint16_t remain_mah, uint16_t full_mah,
                           bool wifi_connected, const char *wifi_ip)
{
    s_temp = temp; s_hum = humidity; s_lux = lux;
    s_bat_mv = battery_mv; s_bat_pct = battery_pct;
    s_bat_temp = bat_temp_c;
    s_current_ma = current_ma;
    s_remain_mah = remain_mah; s_full_mah = full_mah;
    s_wifi_ok = wifi_connected;
    if (wifi_ip) snprintf(s_wifi_ip, sizeof(s_wifi_ip), "%s", wifi_ip);
}

void home_screen_set_chat(const char *text)
{
    if (!text) return;
    snprintf(s_chat_full, sizeof(s_chat_full), "%s", text);
    s_chat_pos = 0;
    s_display_buf[0] = '\0';
    s_chat_active = true;
    lv_timer_create(chat_render_timer, 50, NULL);  /* 50ms/char */
}

void home_screen_append_chat(const char *text, bool first)
{
    /* Legacy streaming support — just buffer and display */
    if (!text) return;
    if (first) s_chat_full[0] = '\0';
    int cur = strlen(s_chat_full);
    snprintf(s_chat_full + cur, sizeof(s_chat_full) - cur, "%s", text);
    s_chat_pos = strlen(s_chat_full);
    s_chat_active = false;
    if (lbl_chat) lv_label_set_text(lbl_chat, s_chat_full);
}
