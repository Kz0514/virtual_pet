/**
 * @file event_bus.h
 * @brief 内部事件总线接口
 */
#pragma once
#include "esp_err.h"
#include <stdint.h>

/** 事件类型枚举 */
typedef enum {
    EVENT_NONE = 0,
    EVENT_TOUCH,           /* 触摸手势 */
    EVENT_SHAKE,           /* 摇动检测 (MPU6500) */
    EVENT_WAKE_WORD,       /* 唤醒词检测 */
    EVENT_SENSOR,          /* 传感器阈值触发 */
    EVENT_SERVER_MSG,      /* 服务器推送消息 */
    EVENT_TIMER,           /* 定时器到期 */
    EVENT_BATTERY_LOW,     /* 低电量 */
    EVENT_NETWORK_STATE,   /* 网络状态变更 */
    EVENT_PET_STATE,       /* 宠物状态变更 */
} event_type_t;

esp_err_t event_bus_init(void);
esp_err_t event_bus_publish(event_type_t type, void *data);
