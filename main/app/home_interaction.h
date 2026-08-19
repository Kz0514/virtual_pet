/** @file home_interaction.h @brief 主页面交互集中组件 — 摇动/敲击/摸头/语音/静音
 *
 * 所有"主页才会发生的交互反应"集中在此, 由 input_handler 按页面启停:
 * 禁用时 poll 只排空检测器事件(不产生任何宠物反应), 手势事件直接忽略。
 */
#pragma once
#include <stdbool.h>
#include "gesture_detect.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 页面级开关 — 由 input_handler 在页面切换时调用 */
void home_interaction_set_enabled(bool en);

/** 主循环 (~500ms) 调用: 摇动/敲击检测分发。
 *  禁用或马达震动期间只排空事件, 不处理。 */
void home_interaction_poll(void);

/** 手势路由入口 (仅主页交互): PETTING_HEAD / VOICE_TRIGGER / MUTE_TOGGLE */
void home_interaction_on_gesture(gesture_event_t ev);

#ifdef __cplusplus
}
#endif
