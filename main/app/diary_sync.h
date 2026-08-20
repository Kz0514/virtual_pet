/**
 * @file diary_sync.h
 * @brief 日记 HTML 同步 — 服务端渲染 → /data/diary/YYYY-MM-DD.html (USB 直读)
 */
#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 创建同步任务 (不阻塞, 无网络时静默) */
esp_err_t diary_sync_init(void);

/** main.c 2s 节拍调用 — 内部判定触发条件与节流 (6h) */
void diary_sync_tick(void);

#ifdef __cplusplus
}
#endif
