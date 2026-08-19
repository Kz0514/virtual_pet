/**
 * @file pet_engine.h
 * @brief 萝莉丝宠物状态机 — 等级/经验/心情/饥饿
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 交互事件 ── */
typedef enum {
    PET_EVENT_TOUCH,
    PET_EVENT_DOUBLE_TOUCH,
    PET_EVENT_SHAKE,
    PET_EVENT_HARD_SHAKE,   /* 重摇 (|a| ≥ 0.8g) */
    PET_EVENT_VOICE,
    PET_EVENT_FEED,
    PET_EVENT_PRAISE,
    PET_EVENT_SCOLD,
    PET_EVENT_LONG_IDLE,
    PET_EVENT_COUNT
} pet_event_t;

/* ── 表情 ── */
typedef enum {
    PET_FACE_NEUTRAL,
    PET_FACE_HAPPY,
    PET_FACE_VERY_HAPPY,
    PET_FACE_SAD,
    PET_FACE_SLEEPY,
    PET_FACE_EXCITED,
    PET_FACE_EATING,
    PET_FACE_SURPRISED,
} pet_face_t;

/* ── 状态快照 ── */
typedef struct {
    uint8_t  level;          /* 当前等级，1 起始，无上限 */
    uint32_t exp;            /* 当前等级内经验值 */
    uint32_t exp_to_next;    /* 升到下一级所需经验 */
    uint8_t  mood;           /* 心情 0-100 */
    uint8_t  hunger;         /* 饥饿 0-100 */
    uint8_t  energy;         /* 精力 0-100 */
    uint8_t  personality;    /* 性格 (预留) */
    pet_face_t face;
    uint32_t age_seconds;
} pet_state_t;

/* ── API ── */

esp_err_t pet_engine_init(void);
void pet_engine_tick(uint32_t tick_ms);
void pet_engine_trigger(pet_event_t event);
pet_state_t pet_engine_get_state(void);
void pet_engine_save(void);

/** 加经验 — 自动应用心情倍率: final = base_exp × (1 + mood/100)，取整 */
void pet_add_exp(int32_t base_exp);
/** 扣经验 — 不带倍率。exp<0 时降级 */
void pet_remove_exp(int32_t exp);
/** 变更心情，自动限幅 0-100 */
void pet_add_mood(int8_t delta);
/** 变更饥饿，自动限幅 0-100 */
void pet_add_hunger(int8_t delta);
/** LLM 聊天结果: mood_d(-3~+10), exp_d(-1~+5)，可能触发彩蛋 */
void pet_process_chat(int8_t mood_d, int8_t exp_d);
/** 查询升级所需经验 */
uint32_t pet_get_exp_to_next(void);
/** 获取心情标签 */
const char* pet_get_mood_label(void);
/** 常用时段 (预留接口，暂返回 true) */
bool pet_is_active_hours(void);

typedef void (*pet_face_cb_t)(pet_face_t face);
void pet_engine_on_face_change(pet_face_cb_t cb);

#ifdef __cplusplus
}
#endif
