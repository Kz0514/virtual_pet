/** @file pat_detector.h @brief 摸头按住检测 — 顶部滑条中间三电极按住 → motou 循环动画 */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化摸头检测器 — 内部 LVGL 定时器 (20ms) 轮询触摸状态.
 *  依赖 touch_fpc_init() / pet_avatar_init() 已就绪.
 *  行为: 中间三电极任一按住 → 循环播 motou; 手指离开 200ms → 切回 idle. */
void pat_detector_init(void);

/** 摸头动画是否正在播放 (供表情映射等判断, 避免抢占) */
bool pat_detector_is_active(void);

/** 页面级开关: 禁用时立即释放按住状态并回 idle (设置页导航电极
 *  与摸头电极物理重叠, 进场前必须禁用) */
void pat_detector_set_enabled(bool en);

#ifdef __cplusplus
}
#endif
