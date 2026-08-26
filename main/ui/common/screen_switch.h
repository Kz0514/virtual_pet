/**
 * @file screen_switch.h
 * @brief 屏幕切换: 强制全屏重绘 (残影防护)
 *
 * 部分刷新模式下, 屏幕切换后的第一帧若不完整覆盖全部行, 未刷新的行会
 * 残留旧屏幕像素。screen_load_full 在加载期间禁用 invalidate (新屏幕
 * 创建期的 invalidate 本就无效), 加载后强制整屏重绘, 保证首帧完整。
 */
#pragma once

#include "lvgl.h"

/**
 * @brief 加载屏幕并强制全屏重绘
 * @param scr 新屏幕对象 (不检查 NULL, 调用方保证)
 */
void screen_load_full(lv_obj_t *scr);
