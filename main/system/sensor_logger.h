/** @file sensor_logger.h @brief 传感器数据日志 — 最近3次读数 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "wear_levelling.h"

/** 一次传感器快照 */
typedef struct {
    uint32_t timestamp;    /* Unix epoch seconds */
    float    temperature;  /* Celsius */
    uint8_t  humidity;     /* % */
    float    ambient_lux;  /* lux */
    int      battery_mv;   /* mV */
    uint8_t  battery_pct;  /* % */
} sensor_snapshot_t;

/** 初始化 (挂载 /cfg LittleFS; /data 经 usb_storage 挂载) */
esp_err_t sensor_logger_init(void);

/** /data (用户可见分区) 是否可用 — life_log / diary_sync / USB 模式查询 */
bool sensor_logger_data_mounted(void);

/** data 分区 WL 句柄 (FAT 挂载由 usb_storage 统一管理) */
wl_handle_t sensor_logger_get_wl_handle(void);

/** 追加一条读数, 自动裁剪到最近 3 条 */
esp_err_t sensor_logger_append(const sensor_snapshot_t *s);

/** 获取最近 n 条 (最多 3) */
int sensor_logger_get_recent(sensor_snapshot_t *out, int max_count);

/** 获取时间戳格式化字符串 */
void sensor_logger_format_time(uint32_t unix_sec, char *buf, int bufsize);

/** 获取最近一条传感器数据作为 LLM 上下文字符串 */
int sensor_logger_get_context_str(char *buf, int bufsize);
