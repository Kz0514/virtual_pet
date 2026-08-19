/** @file memory_store.h @brief 设备端对话记忆 — /data 分区 memory.txt (上限 100KB) */
#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化记忆文件 (依赖 sensor_logger_init 先挂载 /data) */
esp_err_t memory_store_init(void);

/** 追加一轮对话 (主人/萝莉丝各一行); 超 100KB 仅告警, 压缩由服务端负责 */
esp_err_t memory_store_append(const char *user, const char *assistant);

/** 读整个记忆文件到 PSRAM 堆内存 (调用方 free) */
esp_err_t memory_store_get(char **out_buf, size_t *out_len);

/** 整文件覆盖 (服务端压缩后下发); TTS 播放中暂存 pending, 节拍冲刷时落盘 */
esp_err_t memory_store_overwrite(const char *content);

/** 主循环 2s 节拍调用: TTS 空闲时冲刷 pending 覆盖 + 刷新元数据缓存 */
void memory_store_tick(void);

/** 当前文件字节数 (直接 stat, 勿在窄栈任务里调用) */
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
