/**
 * 挠头动画清单 — 命名规范: 所有符号以文件夹名 "naotou_" 为前缀
 * 帧文件: /spiffs/naotou_00.bin .. naotou_01.bin
 *
 * 每个动画文件夹各有一个 manifest.h，通过前缀避免符号冲突。
 */
#pragma once
#include <stdint.h>

/* pet_avatar.h 中的 avatar_frame_t 与此结构兼容 */
typedef struct {
    uint8_t  frame_idx;
    uint16_t duration_ms;   /* 0=使用全局默认FPS */
    uint8_t  loop_back;     /* 子循环回跳步数 (0=正常) */
    uint8_t  loop_extra;    /* 额外循环次数 */
} naotou_anim_frame_t;

/* 帧数 */
#define NAOTOU_FRAME_COUNT  2

/* 默认全局帧率 (仅对 duration_ms=0 的帧生效) */
#define NAOTOU_DEFAULT_FPS  8

/* 帧序表 — 可编辑顺序和每帧时长 */
static const naotou_anim_frame_t naotou_sequence[] = {
    { 0, 1500, 0, 0},  /* 帧0: 1500ms */
    { 1, 300, 0, 0},   /* 帧1: 300ms (1x) */
};
#define NAOTOU_SEQ_LEN  (sizeof(naotou_sequence) / sizeof(naotou_sequence[0]))

