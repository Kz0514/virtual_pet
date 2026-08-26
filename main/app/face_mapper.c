/** @file face_mapper.c
 * @brief 表情出口 — 把 pet_face_t 映射为对应动画
 * (由 pet_engine_on_face_change 回调触发).
 *
 * 只在宠物 idle 且未被摸头时应用: LLM 对话动画与物理交互动画播完回 idle 后,
 * 下一次 tick 的表情重算会自动接管, 不与更高优先级动画互相抢占. */
#include "face_mapper.h"
#include "pet_engine.h"
#include "pet_avatar.h"
#include "pat_detector.h"

static void face_cb(pet_face_t face)
{
    if (pat_detector_is_active()) return;                     /* 摸头 hold 期间不抢 */
    if (pet_avatar_get_current() != PET_ANIM_IDLE) return;    /* 动画播完前不覆盖 */

    static const pet_anim_t map[] = {
        [PET_FACE_NEUTRAL]    = PET_ANIM_IDLE,
        [PET_FACE_HAPPY]      = PET_ANIM_HAPPY,
        [PET_FACE_VERY_HAPPY] = PET_ANIM_EXCITED,
        [PET_FACE_SAD]        = PET_ANIM_SAD,
        [PET_FACE_SLEEPY]     = PET_ANIM_SLEEPY,
        [PET_FACE_EXCITED]    = PET_ANIM_EXCITED,
        [PET_FACE_EATING]     = PET_ANIM_EATING,
        [PET_FACE_SURPRISED]  = PET_ANIM_SURPRISED,
    };
    pet_avatar_play(map[face]);
}

void face_mapper_init(void)
{
    pet_engine_on_face_change(face_cb);
}
