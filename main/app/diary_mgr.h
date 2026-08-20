/**
 * @file diary_mgr.h
 * @brief 日记互动上报 + 今日计数
 *
 * 摸头/语音是日记"互动次数"的来源之一(服务端日记阈值判定):
 *  - 联网时上报 sensor_event (petting/voice), 服务端落库;
 *  - 本机计数今日互动次数 (diary_day=YYYYMMDD + diary_cnt, 跨天清零),
 *    供未来主屏展示"今日互动"; 写盘受 memory_store_writes_safe 闸管控。
 */
#pragma once
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DIARY_EVENT_PETTING = 0,   /* 摸头 */
    DIARY_EVENT_VOICE,         /* 语音会话 */
} diary_event_t;

/** 初始化 (依赖 config_mgr_init / time_manager_init 已执行) */
esp_err_t diary_mgr_init(void);

/** 记录一次互动: 同事件 3s 节流; 联网时上报, 未联网只计数 */
void diary_mgr_note_event(diary_event_t ev);

/** 今日互动计数 (YYYYMMDD 跨天自动清零; 未校时返回 0) */
uint32_t diary_mgr_today_count(void);

#ifdef __cplusplus
}
#endif
