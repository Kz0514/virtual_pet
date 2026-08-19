/** @file input_handler.h @brief 页面交互仲裁器 — 手势事件的唯一路由
 *
 * 触摸 → gesture_detect(驱动层状态机) → input_handler(按页面分发):
 *   HOME 事件     → home_interaction
 *   SETTINGS 事件 → settings_screen_input (第3步接入)
 * 页面切换只调 input_handler_set_page(), 一次性启停该页全部交互。
 */
#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_PAGE_HOME = 0,
    APP_PAGE_SETTINGS,
} app_page_t;

/** 注册手势回调 + 创建 20ms 手势定时器 (替代原 main.c 的注册)。
 *  须在 LVGL 就绪后调用 (app_main, 与原 gesture_set_event_handler 同一位置)。 */
esp_err_t input_handler_init(void);

/** 切换页面交互配置 (LVGL 任务上下文)。
 *  SETTINGS: 禁摇动/敲击/摸头/语音/静音, 左键进入菜单语义
 *  HOME:     全部恢复 */
void input_handler_set_page(app_page_t page);

/** 当前页面 — 内部会先与 settings_screen_is_active() 对账
 *  (旧设置页可能经遗留路径自毁) */
app_page_t input_handler_get_page(void);

#ifdef __cplusplus
}
#endif
