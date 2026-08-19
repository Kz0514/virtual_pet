/** @file settings_screen.h @brief 设置界面 — 子页面模型, 无自轮询
 *
 * 交互全部经 settings_screen_input() 注入 (input_handler 按选择逻辑路由):
 *   UP/DOWN:  点击模式=右滑条轻点上/下半段; 滑动模式=右滑条上/下滑动
 *   CONFIRM:  左键单击(恒成立); 点击模式顶条轻点右半区; 顶条左滑
 *   BACK:     顶条右滑(恒成立); 点击模式顶条轻点左半区
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SETTINGS_EV_UP = 0,     /* 上一项 / 数值加 */
    SETTINGS_EV_DOWN,       /* 下一项 / 数值减 */
    SETTINGS_EV_CONFIRM,    /* 进入子页 / 进出调值 / 切换开关 */
    SETTINGS_EV_BACK,       /* 退出调值 / 回上级 / 退出设置 */
} settings_event_t;

/** 创建并显示设置界面 (根页) */
esp_err_t settings_screen_init(void);

/** 销毁设置界面 (不负责切屏 — 由 close_cb 的宿主处理) */
void settings_screen_destroy(void);

/** 设置界面是否当前显示中 */
bool settings_screen_is_active(void);

/** 唯一输入口 — 由 input_handler 在 LVGL 上下文调用 */
void settings_screen_input(settings_event_t ev);

/** 注册"退出设置"回调 (根页 BACK 时调用; input_handler 负责切回主页) */
void settings_screen_set_close_cb(void (*cb)(void));

#ifdef __cplusplus
}
#endif
