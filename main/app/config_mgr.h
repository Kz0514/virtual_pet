/**
 * @file config_mgr.h
 * @brief 配置持久化 — NVS namespace "settings" 的 u32 键值存取
 *
 * 低频设置项(亮度/息屏时长/功能开关等)。get 带 RAM 缓存,
 * set 立即写 NVS。调用频率为人工调值节奏, 写盘极少。
 */
#pragma once
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化配置管理(清空 RAM 缓存; NVS 需已就绪) */
esp_err_t config_mgr_init(void);

/** 读取 u32 配置; 不存在时返回 def 并缓存 */
uint32_t config_get_u32(const char *key, uint32_t def);

/** 写入 u32 配置(RAM 缓存 + NVS 立即落盘) */
void config_set_u32(const char *key, uint32_t val);

/** 字符串配置最大字节数(UTF-8, 15 个 CJK 字 ≈45B, 留余量) */
#define CFG_STR_MAX 64

/**
 * 读取字符串配置; 不存在/超长时返回 def 并缓存。
 * 返回指向静态缓存, 有效期至该键下次 set — 调用方应立即拷贝。
 * 首次调用会 nvs_open 读 flash, 严禁在 PSRAM 栈任务/临界区内触发
 * (需在 init 期预热); 预热后为纯 RAM 读。
 */
const char *config_get_str(const char *key, const char *def);

/** 写入字符串配置(RAM 缓存 + NVS 立即落盘, 截断到 CFG_STR_MAX-1) */
void config_set_str(const char *key, const char *val);

#ifdef __cplusplus
}
#endif
