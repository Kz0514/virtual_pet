/** @file pat_detector.h @brief 摸头滑动检测 — 顶部滑条来回滑动 → motou 循环动画
 *
 * 触发语义: 惰性"撸猫"— 顶部滑条上往返滑动 (方向翻转 + 单程行程门限)
 * 才触发; 按在滑条上不动或离手 200ms 结束。息屏滑动先唤醒再摸头。
 */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 摸头状态回调 — true=摸头开始 (滑动触发), false=结束 (停手/离手/禁用)
 *  注册后可接收状态翻转; 未注册时行为不变 (纯检测器通知口)。 */
typedef void (*pat_state_cb_t)(bool active, void *ctx);

/** 初始化摸头检测器 — 内部 LVGL 定时器 (20ms) 轮询触摸状态.
 *  依赖 touch_fpc_init() / pet_avatar_init() 已就绪.
 *  行为: 顶部滑条来回滑动 → 循环播 motou + 持续轻震; 无活动 200ms → 切回 idle. */
void pat_detector_init(void);

/** 注册摸头状态回调 (③-2 起供 home_interaction 承接反应) */
void pat_detector_set_cb(pat_state_cb_t cb, void *ctx);

/** 摸头动画是否正在播放 (供表情映射等判断, 避免抢占) */
bool pat_detector_is_active(void);

/** 页面级开关: 禁用时立即释放按住状态并回 idle (设置页导航电极
 *  与摸头电极物理重叠, 进场前必须禁用) */
void pat_detector_set_enabled(bool en);

#ifdef __cplusplus
}
#endif
