/**
 * @file home_screen.c
 * @brief 主屏幕 — 宠物主页面的组装与恢复
 *
 * 主页由五个 widget 组成: pet_avatar(宠物动画) + status_bar(状态栏) +
 * chat_bubble(对话气泡) + notify_overlay(通知浮层) + brightness_bar(亮度条),
 * 组装在 app_main 初始化阶段由 home_screen_init() 完成 (自持 LVGL 锁)。
 */
#include "home_screen.h"
#include "loading_screen.h"
#include "screen_switch.h"
#include "pet_avatar.h"
#include "status_bar.h"
#include "chat_bubble.h"
#include "notify_overlay.h"
#include "brightness_bar.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "home";

/* 主屏幕引用 (供设置等子界面返回时恢复) — 初始化时取 lv_scr_act() */
static lv_obj_t *s_main_scr = NULL;

/* 屏幕节能 API 由 main.c 提供 (③-4 将归入 ui/screen_power.h 正式声明) */
extern void main_screen_note_interaction(void);

esp_err_t home_screen_init(void)
{
    /* ── 初始化完成: 销毁加载界面, 创建真实 UI ──
     * 整段持 LVGL 锁: UI 树构建期间若与渲染任务并发, invalidate
     * 撞上 rendering_in_progress 会断言死循环 (递归锁, 嵌套安全) */
    lvgl_port_lock(0);
    loading_screen_destroy();
    pet_avatar_init();
    /* boot 预加载摸头动画 (内部堆充足期) — 首次摸头零延迟 */
    {
        extern void pet_avatar_preload(void);
        pet_avatar_preload();
    }
    status_bar_init();
    chat_bubble_init();
    notify_overlay_init();
    brightness_bar_init();
    s_main_scr = lv_scr_act();
    lvgl_port_unlock();

    ESP_LOGI(TAG, "主屏幕就绪");
    return ESP_OK;
}

void home_screen_restore(void)
{
    if (s_main_scr) {
        lvgl_port_lock(0); /* main 线程调 lv_ API 必须持锁 (见 loading_screen.c 注释) */
        screen_load_full(s_main_scr);
        lvgl_port_unlock();
        main_screen_note_interaction();
    }
}