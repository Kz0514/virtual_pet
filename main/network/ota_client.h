#pragma once
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Check for firmware update and apply if available. Returns true if updated. */
bool ota_client_check_and_update(const char *token, const char *current_version);

/** OTA check 同步执行 — 注册成功后主循环直接调用。
 * 曾用独立任务 xTaskCreate, 但任务创建/调度在部分启动场景不可靠,
 * 导致 check 从未发出、OTA 永远不触发; 改为与 register 同上下文同步执行。 */
void ota_client_check_sync(void);

/** FreeRTOS task entry for OTA check (doesn't block main loop) */
void ota_check_task(void *pvParameter);

/** 设置页"检查更新": 置请求标志, 主循环 ota_client_check_poll 同步执行。
 * 与开机检查同上下文 — 规避独立任务创建在部分启动场景不可靠的老坑。 */
void ota_client_request_check(void);

/** 主循环每 ~500ms 调用: 有手动请求则执行检查 (带 overlay 进度反馈)。
 * 阻塞主循环期间 LVGL 任务照常渲染, 下载进度可显示。 */
void ota_client_check_poll(void);

#ifdef __cplusplus
}
#endif
