/**
 * @file pet_avatar.h
 * @brief 宠物角色全帧动画视图 (240×240 RGB565, SPIFFS→PSRAM)
 *
 * 素材: assets/animation/<name>/ 下的 .bin 帧文件 + manifest.h
 * 每个动画文件夹包含: manifest.h (帧序表) + 帧二进制文件
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 帧描述符 (与 manifest.h 中的 anim_frame_t 对应) ── */
typedef struct {
    uint8_t  frame_idx;      /* 帧池中的序号 */
    uint16_t duration_ms;    /* 此帧持续时间 (0=使用默认FPS) */
    uint8_t  loop_back;      /* 子循环回跳步数 (0=正常; N=回跳N帧到循环起点) */
    uint8_t  loop_extra;     /* 额外循环次数 (总次数=1+loop_extra) */
} avatar_frame_t;

/* ── 动画序列 ID ── */
typedef enum {
    PET_ANIM_IDLE,       /* 待机 zhanli */
    PET_ANIM_HAPPY,      /* 开心 gaoxingjiangjie */
    PET_ANIM_SAD,        /* 难过 (复用zhanli) */
    PET_ANIM_EXCITED,    /* 兴奋 baoxiongshuohua */
    PET_ANIM_SLEEPY,     /* 困倦 shuijiao */
    PET_ANIM_EATING,     /* 吃东西 e */
    PET_ANIM_SURPRISED,  /* 惊讶 dunzhe */
    PET_ANIM_BLUSH,      /* 害羞 miantianxiao */
    PET_ANIM_PATHEAD,    /* 摸头 motou */
    PET_ANIM_SCRATCH,    /* 挠头 naotou */
    PET_ANIM_POINTSELF,  /* 指着自己 zhizheziji */
    PET_ANIM_COUNT
} pet_anim_t;

/** 初始化动画系统 */
esp_err_t pet_avatar_init(void);

/** 切换到指定动画序列 */
void pet_avatar_play(pet_anim_t anim);

/** 运行时调整帧率 (0=恢复默认). 例如 pet_avatar_set_fps(12) */
void pet_avatar_set_fps(uint8_t fps);

/** 运行时替换当前动画的帧序表 (从 manifest.h 加载).
 *  @param frames  帧序数组 (可随时修改)
 *  @param count   数组长度
 *  @param loop    是否循环 (0=播放一次停止, 1=循环) */
void pet_avatar_set_sequence(const avatar_frame_t *frames, uint8_t count, uint8_t loop);

/** 设置指定动画的子循环额外次数 (覆盖 manifest 中的 loop_extra).
 *  @param anim  动画 ID
 *  @param extra 额外循环次数 (总次数=1+extra); 0=不循环子组 */
void pet_avatar_set_sub_loop(pet_anim_t anim, uint8_t extra);

/** 保持模式: 开启后当前动画的子循环不再计数、播完也不回 idle —
 *  用于"按住期间循环播放" (如摸头), 关闭后由调用方切回 idle */
void pet_avatar_set_hold(bool on);

/** 立即播放 (跳过最小延迟) — 用于物理交互等需即时响应的场景 (如摸头) */
void pet_avatar_play_fast(pet_anim_t anim);

/** 当前正在播放的动画 */
pet_anim_t pet_avatar_get_current(void);

#ifdef __cplusplus
}
#endif
