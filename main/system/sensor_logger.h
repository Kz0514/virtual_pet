/** @file sensor_logger.h @brief 传感器数据日志 — 最近3次读数 */
#pragma once
#include <stdint.h>
#include "esp_err.h"

/** 一次传感器快照 */
typedef struct {
    uint32_t timestamp;    /* Unix epoch seconds */
    float    temperature;  /* Celsius */
    uint8_t  humidity;     /* % */
    float    ambient_lux;  /* lux */
    int      battery_mv;   /* mV */
    uint8_t  battery_pct;  /* % */
} sensor_snapshot_t;

/** 初始化 (挂载 FatFS) */
esp_err_t sensor_logger_init(void);

/** 追加一条读数, 自动裁剪到最近 3 条 */
esp_err_t sensor_logger_append(const sensor_snapshot_t *s);

/** 获取最近 n 条 (最多 3) */
int sensor_logger_get_recent(sensor_snapshot_t *out, int max_count);

/** 获取时间戳格式化字符串 */
void sensor_logger_format_time(uint32_t unix_sec, char *buf, int bufsize);

/** 获取最近一条传感器数据作为 LLM 上下文字符串 */
int sensor_logger_get_context_str(char *buf, int bufsize);
