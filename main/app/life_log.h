/** @file life_log.h @brief 全量交互日志 — 用户可见的"生命记录" (USB 直读) */
#pragma once
#include "esp_err.h"

/** 初始化日志任务与队列 (sensor_logger_init 后调用) */
esp_err_t life_log_init(void);

/**
 * 追加一行交互日志 (带时间戳, 写 /data/life/log.txt)。
 * 调用方零阻塞 — 文本入队由专用任务落盘; 队列满/分区不可用时静默丢弃。
 */
void life_log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
