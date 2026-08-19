/** @file noise_detector.h @brief 环境噪音检测 + 层级时间窗平均值 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 时间窗层级索引 */
typedef enum {
    NOISE_WIN_5S = 0,
    NOISE_WIN_10S,
    NOISE_WIN_30S,
    NOISE_WIN_60S,
    NOISE_WIN_3MIN,
    NOISE_WIN_10MIN,
    NOISE_WIN_20MIN,
    NOISE_WIN_1H,
    NOISE_WIN_3H,
    NOISE_WIN_8H,
    NOISE_WIN_24H,
    NOISE_WIN_3DAY,
    NOISE_WIN_COUNT
} noise_window_t;

/** 初始化噪音检测器 (需 ES8311 已初始化, FatFS data 分区已挂载) */
void noise_detector_init(void);

/** 获取最近一次原始噪音等级 (0-100) */
uint8_t noise_detector_get_level(void);

/** 获取指定时间窗的平均噪音等级 (0-100), 无数据返回 0 */
uint8_t noise_detector_get_avg(noise_window_t win);

/** 是否超过阈值 (默认 60) */
bool noise_detector_is_loud(void);

/** 通知有新 raw 样本 (由 noise_task 内部调用) */
void noise_detector_feed(uint8_t level);

/** 将层级平均值写入 CSV (主循环调用, 线程安全) */
void noise_detector_write_csv(void);

/** 格式化噪音数据为 LLM 上下文字符串 (5s/1h/24h) */
void noise_detector_get_context_str(char *buf, int bufsize);

#ifdef __cplusplus
}
#endif
