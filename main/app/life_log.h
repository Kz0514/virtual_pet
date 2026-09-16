/** @file life_log.h @brief 全量交互日志 — 用户可见的"生命记录" (USB 直读) */
#pragma once
#include "esp_err.h"

/** 建 /data/life 目录并打印就绪 (data_writer_init 后调用; 落盘侧也会补建) */
esp_err_t life_log_init(void);

/**
 * 追加一行交互日志 (时间戳 = **本调用时刻**, 写 /data/life/log.txt)。
 * 调用方零阻塞 — 只格式化 + 投给 data_writer 攒批 (DW_LIFE_LOG, 30s 落盘);
 * 缓冲满/未初始化/缓冲不足时静默丢弃并计入 data_writer 的 drop 统计。
 * 落盘时机由 data_writer 统一闸门决定 (TTS 播放 / U盘模式 / 低电 / 卷坏
 * 期间不落盘, 条件恢复后补写)。
 */
void life_log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
