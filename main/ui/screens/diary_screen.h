/** @file diary_screen.h @brief 日记列表/详情界面接口 */
#pragma once
#include "esp_err.h"
#include "settings_screen.h" /* settings_event_t — 输入枚举复用设置页 */

#ifdef __cplusplus
extern "C" {
#endif

/** 创建并显示日记界面 (列表页); 打开前须为设置页 (BACK 回设置页) */
esp_err_t diary_screen_init(void);

/** 销毁日记界面 (不负责切屏 — 由 input_handler 的 close_cb 处理) */
void diary_screen_destroy(void);

/** 日记界面是否当前显示中 */
bool diary_screen_is_active(void);

/** 唯一输入口 — 由 input_handler 在 LVGL 上下文调用 */
void diary_screen_input(settings_event_t ev);

#ifdef __cplusplus
}
#endif
