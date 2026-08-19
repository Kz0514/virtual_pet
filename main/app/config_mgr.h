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

#ifdef __cplusplus
}
#endif
