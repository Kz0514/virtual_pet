/**
 * @file time_manager.h
 * @brief NTP 时间同步 + 时区管理 接口
 *
 * init 即启动 SNTP 轮询 (lwIP 按 UPDATE_DELAY 自动重试),
 * 成功经 sync_cb 回调置同步标志; 未同步时 get_unix_sec 返回 0。
 * 时区: init 时按 config (tz_auto/tz_manual_sec) 应用初始值,
 * 之后由每日拉取 (time_manager_daily_tz_tick) 更新自动时区。
 */
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 设置 TZ + 启动 SNTP (仅 app_main 调一次) */
esp_err_t time_manager_init(void);

/** 应用时区偏移 (秒, 东八区=28800) — 生成 POSIX TZ 串并立即生效 */
void time_manager_apply_tz(int32_t offset_sec);

/**
 * 每日时区拉取 — 主循环每 2s 调用:
 * 距上次拉取 ≥24h 且自动模式开启时, 向服务端查询 IP 定位时区并应用;
 * 拉取失败保持当前时区 (config tz_last 不更新, 下轮重试)。
 * 重启后首次触发即拉取 (tz_last 缺省=0)。
 */
void time_manager_daily_tz_tick(void);

/** 最近一次 SNTP 同步是否成功 */
bool time_manager_is_synced(void);

/** 当前 Unix 秒 (未同步时=0, 调用方自行判断) */
uint32_t time_manager_get_unix_sec(void);

/** 当前生效的时区偏移 (秒) */
int32_t time_manager_get_tz_offset(void);

#ifdef __cplusplus
}
#endif
