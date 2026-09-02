/** @file font_loader.h @brief LittleFS 字体加载 */
#pragma once
#include "lvgl.h"

/** 全局中文字体指针, init 后可用 */
extern lv_font_t * g_zh_font;

/** 中文首选字体: 从 LittleFS 加载 zh.bin, 失败则 NULL */
#define FONT_ZH  (g_zh_font)

/** 从 LittleFS 加载二进制字体 */
void font_loader_init(void);
