/**
 * @file power_diag.h
 * @brief 功耗/睡眠离线诊断 (⑤-1 从 main.c 拆出)
 *
 * power_log.csv (2s 追加, >64KB 重开) + power_seg.csv (256KB 环形 ≈25h)
 * + 息屏锁/timer dump + tasks.txt — 拔电期间串口死, 只能靠这些 CSV/文件
 * 回看功耗与唤醒。fd 路径零分配, 禁 fopen (SRAM 紧张 abort); 唯一例外:
 * 息屏锁 dump (esp_pm_dump_locks/esp_timer_dump 官方 API 硬依赖 FILE*,
 * 内部堆 <8KB 时跳过)。
 */
#pragma once

#include "bq27220.h" /* bq27220_data_t */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 2s 一条追加 /data/power_log.csv (state: 0=亮 1=变暗 2=息屏) */
void power_diag_log_append(const bq27220_data_t *bat, int state);

/** 2s 块调用 (have_bat 时): 累积段统计, 每 60s 落 S 行 */
void power_diag_seg_tick(const bq27220_data_t *bat);

/** 唤醒翻转点调用: 记录 W 行 (power_manager 在唤醒全流程记账) */
void power_diag_note_wake_source(uint8_t wake_src);

/** 2s 节拍内, 与屏幕状态无关: 轻睡窗口 + 统计日志 (30s 一次) */
void power_diag_pm_stats_log(void);

/** 息屏 15s 一次诊断: 轻睡计数 + PM 锁/timer dump + tasks.txt */
void power_diag_screen_off_diag(void);

#ifdef __cplusplus
}
#endif
