/**
 * @file notify_overlay.c
 * @brief 屏幕提示文字 — 无框纯文字浮层, 淡入淡出, 线程安全
 */
#include "notify_overlay.h"
#include "font_loader.h"
#include "esp_log.h"
#include "lvgl.h"
#include <string.h>

static const char *TAG = "notify";

static lv_obj_t   *s_label;
static lv_timer_t *s_poll_timer;
static lv_timer_t *s_fade_timer;

static volatile bool s_show_req;
static volatile bool s_hide_req;
static char          s_text[256];
static uint32_t      s_auto_ms;
static notify_type_t s_type;

static void anim_opa_cb(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, v, 0);
}

static void anim_done_cb(lv_anim_t *a) {
    lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN);
}

static void fade_out(lv_timer_t *t) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_label);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, 500);
    lv_anim_set_completed_cb(&a, anim_done_cb);
    lv_obj_clear_flag(s_label, LV_OBJ_FLAG_HIDDEN);
    lv_anim_start(&a);
    s_fade_timer = NULL;
}

static void poll_cb(lv_timer_t *t) {
    if (s_hide_req) {
        s_hide_req = false;
        s_show_req = false;
        if (s_fade_timer) { lv_timer_delete(s_fade_timer); s_fade_timer = NULL; }
        lv_anim_del(s_label, NULL);
        lv_obj_add_flag(s_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (!s_show_req) return;
    s_show_req = false;

    lv_label_set_text(s_label, s_text);
    lv_color_t tc = lv_color_hex(0x88CCFF);  /* info: 浅蓝 */
    if (s_type == NOTIFY_WARN)  tc = lv_color_hex(0xFFCC44);
    if (s_type == NOTIFY_ERROR) tc = lv_color_hex(0xFF4444);
    lv_obj_set_style_text_color(s_label, tc, 0);
    lv_obj_remove_flag(s_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(s_label, LV_OPA_COVER, 0);

    /* 删除后必须置空 — 曾漏置空导致二次删除已释放定时器
     * (连点"检查更新"→ 悬空指针 → lv_free_core 崩溃重启) */
    if (s_fade_timer) { lv_timer_delete(s_fade_timer); s_fade_timer = NULL; }
    if (s_auto_ms > 0) {
        s_fade_timer = lv_timer_create(fade_out, s_auto_ms, NULL);
        lv_timer_set_repeat_count(s_fade_timer, 1);
    }
}

void notify_overlay_init(void) {
    /* 挂显示器顶层 (display 级 top layer, 跨屏常驻) — 曾用 lv_screen_active()
     * 导致提示只在主界面可见, 设置页里看不到 */
    s_label = lv_label_create(lv_layer_top());
    lv_label_set_long_mode(s_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_label, 150);
    lv_obj_set_pos(s_label, 4, 132);   /* 上调 — 不挡底部对话框 */
    lv_obj_set_style_text_color(s_label, lv_color_hex(0x88CCFF), 0);
    lv_obj_set_style_text_font(s_label, FONT_ZH, 0);
    /* 深色描边提升可读性 (纯文字无背景) */
    lv_obj_set_style_text_opa(s_label, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(s_label, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_label, LV_OBJ_FLAG_HIDDEN);

    s_poll_timer = lv_timer_create(poll_cb, 100, NULL);
    lv_timer_set_repeat_count(s_poll_timer, -1);

    ESP_LOGI(TAG, "提示文字浮层就绪 (无框)");
}

void notify_show(notify_type_t type, const char *text, uint32_t auto_hide_ms) {
    if (!text || !text[0]) return;
    s_type = type;
    strncpy(s_text, text, sizeof(s_text) - 1);
    s_text[sizeof(s_text) - 1] = '\0';
    s_auto_ms = auto_hide_ms;
    s_show_req = true;
}

void notify_hide(void) {
    s_hide_req = true;
}
