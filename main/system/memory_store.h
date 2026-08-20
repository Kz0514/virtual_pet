/** @file memory_store.h @brief 设备端对话记忆 — /cfg 分区 memory.txt (上限 100KB) */
#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化记忆文件 (依赖 sensor_logger_init 先挂载 /cfg) */
esp_err_t memory_store_init(void);

/** 追加一轮对话 (主人/萝莉丝各一行); 超 100KB 仅告警, 压缩由服务端负责。
 * 注意: 调用方若是 PSRAM 栈任务 (如 WS 任务), 本函数必须无 flash 访问 —
 * 判超限用 cached_size (tick 刷新), 实际写盘由专用写盘任务执行 */
esp_err_t memory_store_append(const char *user, const char *assistant);

/** 读记忆结果回调 (写盘任务上下文执行; content 为临时缓冲,
 * 回调返回后即释放, 不得保存指针; 回调内不得再发起 flash 访问) */
typedef void (*memory_read_cb_t)(const char *content, size_t len, void *arg);

/** 异步读整个记忆文件 — 读盘由专用写盘任务执行。PSRAM 栈任务 (WS 任务)
 * 禁止任何同步 flash 访问 (flash 读期间同样禁用 cache, 1.0.213 只移了写,
 * stat/fopen 读照样崩, 1.0.214 修复)。arg 透传给回调, 回调返回后由
 * 调用方负责释放 (如 strdup 的 req_id) */
esp_err_t memory_store_read_async(memory_read_cb_t cb, void *arg);

/** 整文件覆盖 (服务端压缩后下发); 实际写盘由专用写盘任务执行 */
esp_err_t memory_store_overwrite(const char *content);

/** 主循环 2s 节拍调用: 刷新元数据缓存 */
void memory_store_tick(void);

/** 当前文件字节数 (直接 stat — 只在写盘任务/主循环内调用, PSRAM 栈任务勿用) */
size_t memory_store_size(void);

/* ── 元数据缓存: 由 memory_store_tick 刷新, 供 ws_client_send_chat 零 FatFS 使用
 * (FatFS+WL 调用链深, sess 任务 12KB 栈曾栈溢出双异常) ── */

/** 缓存的文件大小 (未刷新过则为 0) */
size_t memory_store_cached_size(void);

/** 缓存的文件内容 (仅 ≤4KB 压缩态), 无则 NULL */
const char *memory_store_cached_summary(void);

/* ── 写盘安全闸: 电池低电压时暂停所有 flash 写 (防断电损坏) ── */
void memory_store_set_writes_safe(bool safe);
bool memory_store_writes_safe(void);

#ifdef __cplusplus
}
#endif
