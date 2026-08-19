/** @file brightness_bar.h @brief 亮度调节指示条 — 自驱动 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

void brightness_bar_init(void);

/** 获取用户当前设定的亮度 (1-100, 默认 80) — 供节能状态机恢复亮度用 */
uint8_t brightness_bar_get(void);

/** 直接设定亮度 (设置页调节用): 更新内部值 + 背光立即生效 */
void brightness_bar_set(uint8_t bri);

/** 主界面亮度条开关 (设置页配置项, 持久化): 关闭后右侧滑条不再调亮度 */
void brightness_bar_set_enabled(bool en);
