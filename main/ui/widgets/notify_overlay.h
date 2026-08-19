/** @file notify_overlay.h @brief 屏幕提示文字 — 无框纯文字浮层 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NOTIFY_INFO = 0,
    NOTIFY_WARN,
    NOTIFY_ERROR,
} notify_type_t;

/** 初始化提示浮层 */
void notify_overlay_init(void);

/** 显示提示 (auto_hide_ms=0 表示常驻) */
void notify_show(notify_type_t type, const char *text, uint32_t auto_hide_ms);

/** 立即隐藏提示 (线程安全, LVGL 轮询消费) */
void notify_hide(void);

#ifdef __cplusplus
}
#endif
