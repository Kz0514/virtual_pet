/**
 * @file ota_screen.c
 * @brief OTA firmware update screen — progress bar + status text
 */
#include "ota_screen.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "ota_screen";

static lv_obj_t *scr = NULL;
static lv_obj_t *bar = NULL;
static lv_obj_t *label = NULL;
static lv_obj_t *pct_label = NULL;

#define C_DARK  lv_color_hex(0x1a1a2e)
#define C_GREEN lv_color_hex(0x4ecca3)
#define C_RED   lv_color_hex(0xe94560)
#define C_WHITE lv_color_hex(0xeeeeee)

esp_err_t ota_screen_init(void)
{
    ESP_LOGI(TAG, "OTA screen init");
    return ESP_OK;
}

static void ensure_screen(void)
{
    if (scr) return;
    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, C_DARK, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Title
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Firmware Update");
    lv_obj_set_style_text_color(title, C_WHITE, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    // Progress bar
    bar = lv_bar_create(scr);
    lv_obj_set_size(bar, 200, 20);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, -10);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_anim_time(bar, 300, 0);

    // Percentage
    pct_label = lv_label_create(scr);
    lv_label_set_text(pct_label, "0%");
    lv_obj_set_style_text_color(pct_label, C_WHITE, 0);
    lv_obj_set_style_text_font(pct_label, &lv_font_montserrat_14, 0);
    lv_obj_align(pct_label, LV_ALIGN_CENTER, 0, 20);

    // Status text
    label = lv_label_create(scr);
    lv_label_set_text(label, "");
    lv_obj_set_style_text_color(label, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 50);

    lv_scr_load(scr);
}

void ota_screen_show_progress(int percent, const char *status)
{
    ensure_screen();

    if (percent < 0) {
        // Error state
        lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar, C_RED, LV_PART_INDICATOR);
        lv_label_set_text(pct_label, "FAIL");
        lv_obj_set_style_text_color(pct_label, C_RED, 0);
    } else {
        lv_bar_set_value(bar, percent, LV_ANIM_ON);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", percent);
        lv_label_set_text(pct_label, buf);
        if (percent >= 100) {
            lv_obj_set_style_bg_color(bar, C_GREEN, LV_PART_INDICATOR);
            lv_obj_set_style_text_color(pct_label, C_GREEN, 0);
        }
    }

    if (status) {
        lv_label_set_text(label, status);
    }
}
