/** @file loading_screen.h @brief 开机加载界面 — "初始化中…" + 转圈动画 */
#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 创建并显示加载界面 (深色背景 + 居中文字 + 旋转弧) */
esp_err_t loading_screen_init(void);

/** 销毁加载界面 (切换到主屏前调用) */
void loading_screen_destroy(void);

#ifdef __cplusplus
}
#endif
