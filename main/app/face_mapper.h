/** @file face_mapper.h @brief 宠物表情 → 动画映射 (表情的视觉出口) */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化表情映射 — 注册 pet_engine_on_face_change 回调.
 *  策略: 仅在宠物 idle 且未摸头时应用表情动画,
 *  避免抢占 LLM 对话动画与物理交互动画. */
void face_mapper_init(void);

#ifdef __cplusplus
}
#endif
