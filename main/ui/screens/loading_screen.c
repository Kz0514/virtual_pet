/**
 * @file loading_screen.c
 * @brief 开机加载界面 — 深色背景 + "初始化中…" + 旋转弧动画
 */
#include "loading_screen.h"
#include "font_loader.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "loading";

static lv_obj_t *s_scr     = NULL;
static lv_obj_t *s_label   = NULL;
static lv_obj_t *s_spinner = NULL;

#define C_DARK  lv_color_hex(0x000000)   /* 纯黑背景 */
#define C_TEXT  lv_color_hex(0xeeeeee)
#define C_ARC   lv_color_hex(0xFFFFFF)   /* 白色弧 */
#define C_RING  lv_color_hex(0x333333)   /* 暗灰底环 */

esp_err_t loading_screen_init(void)
{
    ESP_LOGI(TAG, "创建加载界面…");
    /* main 线程调 lv_ API 必须持 LVGL 递归锁: 否则与渲染任务并发,
     * invalidate 撞上 rendering_in_progress 触发断言死循环 (WDT 复位) */
    lvgl_port_lock(0);

    s_scr = lv_obj_create(NULL);   /* 独立 screen */
    lv_obj_set_style_bg_color(s_scr, C_DARK, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    /* 旋转弧 — 正中间 */
    s_spinner = lv_spinner_create(s_scr);
    lv_obj_set_size(s_spinner, 70, 70);
    lv_obj_set_style_arc_color(s_spinner, C_RING, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_spinner, C_ARC, LV_PART_INDICATOR);
    lv_obj_center(s_spinner);

    /* 居中文字 "Loading" — spinner 下方 */
    s_label = lv_label_create(s_scr);
    lv_label_set_text(s_label, "Loading");
    lv_obj_set_style_text_color(s_label, C_TEXT, 0);
    lv_obj_set_style_text_font(s_label, &lv_font_montserrat_16, 0);
    lv_obj_align(s_label, LV_ALIGN_CENTER, 0, 65);

    lv_scr_load(s_scr);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "加载界面已显示");
    return ESP_OK;
}

void loading_screen_destroy(void)
{
    if (s_scr) {
        lvgl_port_lock(0);
        /* 先切到默认屏幕, 再异步删除 — 避免 LVGL 刷新周期访问已释放对象 */
        lv_obj_t *main_scr = lv_scr_act();
        if (main_scr == s_scr) {
            /* 如果当前还在 loading 屏上, 切到一个空白临时屏 (后续 UI init 会覆盖) */
            lv_obj_t *tmp = lv_obj_create(NULL);
            lv_obj_set_style_bg_color(tmp, lv_color_hex(0x1a1a2e), 0);
            lv_obj_set_style_bg_opa(tmp, LV_OPA_COVER, 0);
            lv_scr_load(tmp);
        }
        lv_obj_del_async(s_scr);   /* 异步删除, 等 LVGL 当前帧渲染完再释放 */
        s_scr = NULL;
        s_label = NULL;
        s_spinner = NULL;
        lvgl_port_unlock();
        ESP_LOGI(TAG, "加载界面已销毁 (async)");
    }
}
