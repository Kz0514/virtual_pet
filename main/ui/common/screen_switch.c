/**
 * @file screen_switch.c
 * @brief 屏幕切换: 强制全屏重绘 (残影防护)
 */
#include "screen_switch.h"
#include "esp_log.h"

static const char *TAG = "screen_sw";

void screen_load_full(lv_obj_t *scr)
{
 if (scr == NULL) return;
 lv_display_t *disp = lv_obj_get_display(scr);

 /* 切换期间禁用 invalidate: 新屏幕创建期对象的 invalidate 均无效
 * (旧屏非 act_scr 时 visible 检查直接丢弃), 集中到加载后一次全屏 */
 lv_display_enable_invalidation(disp, false);
 lv_scr_load(scr);
 lv_display_enable_invalidation(disp, true);

 /* 强制整屏重绘 — 部分刷新模式下切换后首帧必须完整覆盖,
 * 否则未刷新的行残留旧屏幕像素 (残影) */
 lv_obj_invalidate(scr);
 ESP_LOGI(TAG, "已加载屏幕 (强制全屏重绘)");
}
