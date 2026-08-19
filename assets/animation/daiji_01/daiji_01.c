/**
 * 待机动画清单 — 命名规范: 所有符号以文件夹名 "daiji_01_" 为前缀
 * 帧文件: /spiffs/daiji_01_00.bin .. daiji_01_10.bin
 *
 * 每个动画文件夹各有一个 manifest.h，通过前缀避免符号冲突。
 */
#pragma once
#include <stdint.h>

/* pet_avatar.h 中的 avatar_frame_t 与此结构兼容 */
typedef struct {
    uint8_t  frame_idx;
    uint16_t duration_ms;   /* 0=使用全局默认FPS */
    uint8_t  loop_back;   /* 子循环回跳步数 (0=正常) */
    uint8_t  loop_extra;  /* 额外循环次数 */
} daiji_01_anim_frame_t;

/* 帧数 */
#define DAIJI_01_FRAME_COUNT  11

/* 默认全局帧率 (仅对 duration_ms=0 的帧生效) */
#define DAIJI_01_DEFAULT_FPS  8

/* 帧序表 — 可编辑顺序和每帧时长 */
static const daiji_01_anim_frame_t daiji_01_sequence[] = {
    { 0, 300, 0, 0},   /* 帧0: 300ms */
    { 1, 300, 0, 0},   /* 帧1: 300ms */
    { 2, 300, 0, 0},   /* 帧2: 300ms */
    { 3, 300, 0, 0},   /* 帧3: 300ms */
    { 4, 300, 0, 0},   /* 帧4: 300ms */
    { 5, 300, 0, 0},   /* 帧5: 300ms */
    { 6, 300, 0, 0},   /* 帧6: 300ms */
    { 7, 300, 0, 0},   /* 帧7: 300ms */
    { 8, 300, 0, 0},   /* 帧8: 300ms */
    { 9, 300, 0, 0},   /* 帧9: 300ms */
    { 10, 300, 0, 0},  /* 帧10: 300ms */
};
#define DAIJI_01_SEQ_LEN  (sizeof(daiji_01_sequence) / sizeof(daiji_01_sequence[0]))


