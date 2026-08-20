/**
 * @file config_mgr.c
 * @brief 配置持久化 — NVS namespace "settings" 的 u32 键值 + RAM 缓存
 *
 * NVS 写频率极低(人工调值节奏), 不受低电量写盘闸管控。
 * 键名上限 15 字符(NVS 限制)。
 */
#include "config_mgr.h"
#include "nvs.h"
#include "esp_log.h"
#include <stdbool.h>
#include <string.h>

static const char *TAG = "config";

#define CFG_NS       "settings"
/* 现有 4 键 + 时区 3 (tz_auto/tz_manual_sec/tz_last) + 日记 2 (diary_day/diary_cnt) */
#define CFG_MAX_KEYS 12
#define CFG_KEY_LEN  16   /* NVS 键上限 15 + '\0' */

typedef struct {
    char     key[CFG_KEY_LEN];
    uint32_t val;
    bool     valid;
} cfg_entry_t;

static cfg_entry_t s_cache[CFG_MAX_KEYS];

static cfg_entry_t *find_entry(const char *key)
{
    for (int i = 0; i < CFG_MAX_KEYS; i++) {
        if (s_cache[i].valid &&
            strncmp(s_cache[i].key, key, CFG_KEY_LEN) == 0) {
            return &s_cache[i];
        }
    }
    return NULL;
}

static void cache_put(const char *key, uint32_t val)
{
    cfg_entry_t *e = find_entry(key);
    if (e) {
        e->val = val;
        return;
    }
    for (int i = 0; i < CFG_MAX_KEYS; i++) {
        if (!s_cache[i].valid) {
            s_cache[i].valid = true;
            strncpy(s_cache[i].key, key, CFG_KEY_LEN - 1);
            s_cache[i].key[CFG_KEY_LEN - 1] = '\0';
            s_cache[i].val = val;
            return;
        }
    }
    ESP_LOGW(TAG, "缓存已满, %s 不缓存", key);
}

esp_err_t config_mgr_init(void)
{
    memset(s_cache, 0, sizeof(s_cache));
    ESP_LOGI(TAG, "配置持久化就绪 (NVS ns=\"%s\", 缓存 %d 键)",
             CFG_NS, CFG_MAX_KEYS);
    return ESP_OK;
}

uint32_t config_get_u32(const char *key, uint32_t def)
{
    cfg_entry_t *e = find_entry(key);
    if (e) return e->val;

    uint32_t v = def;
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u32(h, key, &v) != ESP_OK) v = def;
        nvs_close(h);
    }
    cache_put(key, v);
    return v;
}

void config_set_u32(const char *key, uint32_t val)
{
    cache_put(key, val);

    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_u32(h, key, val) == ESP_OK) {
            nvs_commit(h);
        } else {
            ESP_LOGW(TAG, "NVS 写 %s 失败", key);
        }
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "NVS 打开失败, %s=%lu 仅在 RAM",
                 key, (unsigned long)val);
    }
}
