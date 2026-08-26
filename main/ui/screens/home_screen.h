/**
 * @file home_screen.h
 * @brief 主屏幕模块 — 宠物主界面 (pet_avatar + status_bar + chat_bubble +
 *        notify_overlay + brightness_bar) 的组装与恢复
 */
#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化主屏幕 (销毁 loading 屏, 组装全部主页 widget, 记录屏幕指针)
 * @note 内部自持 LVGL 锁; 仅 app_main 调用一次
 */
esp_err_t home_screen_init(void);

/**
 * @brief 从子界面 (设置/日记等) 恢复到主屏幕
 * @note main 线程调 lv_ API 必须持锁 (见 loading_screen.c 注释)
 */
void home_screen_restore(void);

#ifdef __cplusplus
}
#endif