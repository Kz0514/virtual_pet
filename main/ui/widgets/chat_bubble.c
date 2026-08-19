/**
 * @file chat_bubble.c
 * @brief 底部对话框 — 逐字显示 + |p 停顿, 线程安全
 */
#include "chat_bubble.h"
#include "font_loader.h"
#include "tts_client.h"
#include "esp_log.h"
#include "lvgl.h"
#include <string.h>

static const char *TAG = "bubble";

static lv_obj_t   *s_bubble;
static lv_obj_t   *s_label;
static lv_timer_t *s_poll_timer;
static lv_timer_t *s_typewriter_tmr;   /* 逐字推进 */
static lv_timer_t *s_auto_hide_tmr;

static volatile bool s_show_req;
static volatile bool s_hide_req;
static volatile bool s_wait_tts;     /* 等TTS开始播放后再逐字显示 */
static volatile bool s_raw_swap;     /* 新文本待交换 (WS线程写pending, LVGL线程交换) */
static char    s_raw[1024];          /* 原始文本 (含|p标记) */
static char    s_raw_pending[1024];  /* 请求侧缓冲 — 消除 WS 写/LVGL 读竞态 */
static volatile uint32_t s_auto_hide_ms = 5000;  /* 默认同 AUTO_HIDE_MS */
static int     s_raw_len;
static int     s_raw_pos;            /* 当前在 raw 中的位置 */
static char    s_visible_text[1024]; /* 已构建的显示文本 (无标记) */
static int     s_visible_len;
static int     s_pause_remain_ms;
static bool    s_visible;

#define TYPEWRITER_INTERVAL_MS  220  /* 每字间隔 — 匹配TTS ~4.5字/秒 */
#define AUTO_HIDE_MS           5000   /* 默认显示后 5s 渐变消失 */

/* ── 动画回调 ── */
static void fade_out_cb(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, v, 0);
}
static void fade_done_cb(lv_anim_t *a) {
    lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
}
static void start_fade_out(void) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_bubble);
    lv_anim_set_exec_cb(&a, fade_out_cb);
    lv_anim_set_values(&a, LV_OPA_90, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, 600);
    lv_anim_set_completed_cb(&a, fade_done_cb);
    lv_obj_clear_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
    lv_anim_start(&a);
}
static void start_fade_out_tmr_cb(lv_timer_t *t) {
    s_auto_hide_tmr = NULL;
    start_fade_out();
}

/* ── 逐字推进 ── */
static void typewriter_cb(lv_timer_t *t) {
    if (s_pause_remain_ms > 0) {
        /* Still in pause, just reset interval */
        lv_timer_set_period(t, TYPEWRITER_INTERVAL_MS);
        s_pause_remain_ms = 0;
        return;
    }

    /* Set speed based on next character type */
    if (s_raw_pos < s_raw_len && (uint8_t)s_raw[s_raw_pos] < 0x80)
        lv_timer_set_period(t, TYPEWRITER_INTERVAL_MS / 4);  /* ASCII fast */
    else
        lv_timer_set_period(t, TYPEWRITER_INTERVAL_MS);

    if (s_raw_pos >= s_raw_len) {
        /* Done */
        lv_timer_delete(s_typewriter_tmr);
        s_typewriter_tmr = NULL;
        if (!s_auto_hide_tmr) {
            s_auto_hide_tmr = lv_timer_create(start_fade_out_tmr_cb,
                                               s_auto_hide_ms, NULL);
            lv_timer_set_repeat_count(s_auto_hide_tmr, 1);
        }
        return;
    }

    /* Check for |pXXX pause marker */
    if (s_raw[s_raw_pos] == '|' && s_raw[s_raw_pos + 1] == 'p') {
        int ms = 0;
        char *p = s_raw + s_raw_pos + 2;
        while (*p >= '0' && *p <= '9') { ms = ms * 10 + (*p - '0'); p++; }
        s_raw_pos = p - s_raw;  /* skip past |pXXX */
        if (ms > 0) {
            s_pause_remain_ms = ms;
            lv_timer_set_period(t, ms);
        }
        return;
    }

    /* Skip [tag] emotion markers */
    if (s_raw[s_raw_pos] == '[') {
        char *end = strchr(s_raw + s_raw_pos, ']');
        if (end) { s_raw_pos = end - s_raw + 1; return; }
    }

    /* Copy one UTF-8 character to visible buffer; speed depends on type */
    uint8_t c = (uint8_t)s_raw[s_raw_pos];
    int clen = 1;
    bool is_ascii = ((c & 0x80) == 0);
    if (is_ascii)                 clen = 1;   /* ASCII */
    else if ((c & 0xE0) == 0xC0)  clen = 2;   /* 2-byte */
    else if ((c & 0xF0) == 0xE0)  clen = 3;   /* 3-byte (CJK) */
    else if ((c & 0xF8) == 0xF0)  clen = 4;   /* 4-byte */

    if (s_visible_len + clen < sizeof(s_visible_text) - 1) {
        memcpy(s_visible_text + s_visible_len, s_raw + s_raw_pos, clen);
        s_visible_len += clen;
        s_visible_text[s_visible_len] = '\0';
    }
    s_raw_pos += clen;

    lv_label_set_text(s_label, s_visible_text);
    lv_obj_update_layout(s_bubble);
    lv_coord_t h = lv_obj_get_height(s_label) + 16;
    if (h < 36) h = 36;
    #define BUBBLE_MAX_H 73  /* 3 lines @16px font */
    if (h > BUBBLE_MAX_H) {
        h = BUBBLE_MAX_H;
        lv_obj_set_scroll_dir(s_bubble, LV_DIR_VER);
        lv_obj_scroll_to_y(s_bubble, LV_COORD_MAX, LV_ANIM_OFF);
    } else {
        lv_obj_set_scroll_dir(s_bubble, LV_DIR_NONE);
    }
    lv_obj_set_height(s_bubble, h);
}

/* ── 轮询 (LVGL 线程) ── */
static void poll_cb(lv_timer_t *t) {
    /* 交换请求侧缓冲 — 只有 LVGL 线程写 s_raw, 消除与 WS 回调的竞态 */
    if (s_raw_swap) {
        s_raw_swap = false;
        strncpy(s_raw, s_raw_pending, sizeof(s_raw) - 1);
        s_raw[sizeof(s_raw) - 1] = '\0';
    }

    if (s_hide_req) {
        s_hide_req = false;
        if (s_typewriter_tmr) { lv_timer_delete(s_typewriter_tmr); s_typewriter_tmr = NULL; }
        if (s_auto_hide_tmr) { lv_timer_delete(s_auto_hide_tmr); s_auto_hide_tmr = NULL; }
        if (s_visible) start_fade_out();
        return;
    }

    if (!s_show_req) return;

    /* 同步模式: 等待 TTS 真正开始播放 */
    if (s_wait_tts) {
        if (tts_client_get_playback_ms() > 0) {
            /* 播放开始 — 开始逐字显示 */
        } else if (!tts_client_is_busy()) {
            /* TTS已结束且没有播放(空音频/失败) — 兜底立即显示 */
            ESP_LOGI(TAG, "TTS无音频, 兜底显示");
        } else {
            return;  /* 还在下载/播放中 — 继续等 */
        }
    }
    s_show_req = false;

    if (s_typewriter_tmr) lv_timer_delete(s_typewriter_tmr);
    if (s_auto_hide_tmr) { lv_timer_delete(s_auto_hide_tmr); s_auto_hide_tmr = NULL; }
    /* Don't fade out old bubble — just replace content immediately */

    s_raw_len = strlen(s_raw);
    s_raw_pos = 0;
    s_visible_len = 0;
    s_visible_text[0] = '\0';
    s_pause_remain_ms = 0;

    lv_obj_remove_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(s_bubble, LV_OPA_90, 0);
    lv_label_set_text(s_label, "");
    lv_obj_set_height(s_bubble, 36);
    s_visible = true;

    s_typewriter_tmr = lv_timer_create(typewriter_cb, TYPEWRITER_INTERVAL_MS, NULL);
    lv_timer_set_repeat_count(s_typewriter_tmr, -1);
}

/* ══════ API ══════ */

void chat_bubble_init(void) {
    s_bubble = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_bubble);
    lv_obj_set_size(s_bubble, 220, LV_SIZE_CONTENT);
    lv_obj_set_pos(s_bubble, 10, 165);
    lv_obj_set_style_bg_color(s_bubble, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_bg_opa(s_bubble, LV_OPA_90, 0);    /* 更不透明 */
    lv_obj_set_style_border_width(s_bubble, 1, 0);
    lv_obj_set_style_border_color(s_bubble, lv_color_hex(0x555588), 0);
    lv_obj_set_style_radius(s_bubble, 12, 0);
    lv_obj_set_style_pad_all(s_bubble, 8, 0);
    lv_obj_set_style_opa(s_bubble, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);

    s_label = lv_label_create(s_bubble);
    lv_label_set_long_mode(s_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_label, 200);
    lv_obj_set_style_text_color(s_label, lv_color_hex(0xEEEEFF), 0);
    lv_obj_set_style_text_font(s_label, FONT_ZH, 0);

    s_poll_timer = lv_timer_create(poll_cb, 100, NULL);
    lv_timer_set_repeat_count(s_poll_timer, -1);

    ESP_LOGI(TAG, "对话框就绪 (逐字)");
}

/* 统一请求入口 — 写 pending 缓冲, 由 poll_cb 在 LVGL 线程交换 */
static void bubble_request(const char *text, uint32_t auto_hide_ms, bool wait_tts) {
    if (!text || !text[0]) return;
    strncpy(s_raw_pending, text, sizeof(s_raw_pending) - 1);
    s_raw_pending[sizeof(s_raw_pending) - 1] = '\0';
    s_auto_hide_ms = auto_hide_ms;
    s_wait_tts = wait_tts;
    s_raw_swap = true;
    s_show_req = true;
}

void chat_bubble_show(const char *text, uint32_t auto_hide_ms) {
    bubble_request(text, auto_hide_ms, false);
}

void chat_bubble_show_sync(const char *text) {
    bubble_request(text, AUTO_HIDE_MS, true);   /* 等 TTS 播放开始再逐字显示 */
}

void chat_bubble_hide(void) {
    s_hide_req = true;
}
