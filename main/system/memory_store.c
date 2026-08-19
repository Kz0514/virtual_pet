/**
 * @file memory_store.c
 * @brief 设备端对话记忆 — /data 分区 memory.txt
 *
 * 完整对话历史存设备端 (上限 100KB), 超限由服务端 LLM 按
 * 信息重要性 + 时间远近压缩后下发覆盖 (memory_update 消息)。
 *
 * /data 由 sensor_logger_init 挂载 (擦除仅在首挂/损坏恢复时,
 * 不再每次开机擦除), 本模块只负责 memory.txt 的读写。
 * 写盘纪律: TTS 播放期间不写盘, 防 flash 冻结卡音频。
 */
#include "memory_store.h"
#include "tts_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

static const char *TAG = "memory";
static const char *MEM_FILE  = "/data/memory.txt";

#define MEM_MAX_BYTES  (100 * 1024)
#define SUMMARY_MAX    (4 * 1024)   /* ≤4KB 视为"压缩态", 随 chat 消息携带 */

static bool  s_ready   = false;
static char *s_pending = NULL;      /* TTS 播放期间暂存的覆盖内容 (PSRAM) */
static bool  s_writes_safe = true;  /* 低电量闸: 电池 <3.7V 暂停写盘 */

void memory_store_set_writes_safe(bool safe) { s_writes_safe = safe; }
bool memory_store_writes_safe(void) { return s_writes_safe; }

/* 元数据缓存 — 主循环 tick 刷新, 发送路径零 FatFS 访问 */
static char  *s_summary = NULL;     /* PSRAM, ≤4KB 压缩态摘要 */
static size_t s_size_cache = 0;
static bool   s_size_valid = false;

static void memory_store_refresh_cache(void);

esp_err_t memory_store_init(void)
{
    /* /data 由 sensor_logger_init 挂载 (main.c 中先于本模块 init) */
    FILE *f = fopen(MEM_FILE, "a");   /* 存在则保持内容, 不存在则创建 */
    if (!f) {
        ESP_LOGW(TAG, "data 分区不可用 — 请确认 sensor_logger_init 已先执行");
        return ESP_FAIL;
    }
    fclose(f);
    s_ready = true;
    memory_store_refresh_cache();
    ESP_LOGI(TAG, "记忆文件就绪, memory.txt %u B", (unsigned)memory_store_size());
    return ESP_OK;
}

size_t memory_store_size(void)
{
    if (!s_ready) return 0;
    struct stat st;
    if (stat(MEM_FILE, &st) != 0) return 0;
    return (size_t)st.st_size;
}

esp_err_t memory_store_append(const char *user, const char *assistant)
{
    if (!s_ready || !s_writes_safe || (!user && !assistant)) return ESP_FAIL;
    if (memory_store_size() > MEM_MAX_BYTES)
        ESP_LOGW(TAG, "记忆已超 100KB, 等待服务端压缩");
    FILE *f = fopen(MEM_FILE, "a");
    if (!f) { ESP_LOGW(TAG, "打开记忆文件失败"); return ESP_FAIL; }
    if (user && user[0])
        fprintf(f, "主人: %s\n", user);
    if (assistant && assistant[0])
        fprintf(f, "萝莉丝: %s\n", assistant);
    fclose(f);
    return ESP_OK;
}

esp_err_t memory_store_get(char **out_buf, size_t *out_len)
{
    if (!s_ready || !out_buf || !out_len) return ESP_FAIL;
    *out_buf = NULL;
    *out_len = 0;
    size_t sz = memory_store_size();
    if (sz == 0) return ESP_OK;   /* 空记忆也算成功 */

    char *buf = heap_caps_malloc(sz + 1, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGW(TAG, "PSRAM 分配失败 (%u B)", (unsigned)sz);
        return ESP_ERR_NO_MEM;
    }
    FILE *f = fopen(MEM_FILE, "r");
    if (!f) { free(buf); return ESP_FAIL; }
    size_t n = fread(buf, 1, sz, f);
    fclose(f);
    buf[n] = '\0';
    *out_buf = buf;
    *out_len = n;
    return ESP_OK;
}

static esp_err_t memory_store_write_now(const char *content)
{
    FILE *f = fopen(MEM_FILE, "w");
    if (!f) { ESP_LOGW(TAG, "打开记忆文件失败"); return ESP_FAIL; }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
    ESP_LOGI(TAG, "记忆已覆盖 (%d B)", (int)strlen(content));
    return ESP_OK;
}

esp_err_t memory_store_overwrite(const char *content)
{
    if (!s_ready || !content) return ESP_FAIL;
    /* TTS 播放期间写盘冻结双核 100-400ms → 音频卡顿, 暂存 PSRAM 等节拍冲刷 */
    if (tts_client_is_playing()) {
        if (s_pending) free(s_pending);
        s_pending = heap_caps_malloc(strlen(content) + 1, MALLOC_CAP_SPIRAM);
        if (!s_pending) return ESP_ERR_NO_MEM;
        strcpy(s_pending, content);
        ESP_LOGI(TAG, "TTS 播放中, 覆盖内容暂存 pending (%d B)", (int)strlen(content));
        return ESP_OK;
    }
    return memory_store_write_now(content);
}

static void memory_store_refresh_cache(void)
{
    s_size_cache = memory_store_size();
    s_size_valid = true;
    if (s_summary) { free(s_summary); s_summary = NULL; }
    if (s_size_cache == 0 || s_size_cache > SUMMARY_MAX) return;  /* 非压缩态 */
    s_summary = heap_caps_malloc(s_size_cache + 1, MALLOC_CAP_SPIRAM);
    if (!s_summary) return;
    FILE *f = fopen(MEM_FILE, "r");
    if (!f) { free(s_summary); s_summary = NULL; return; }
    size_t n = fread(s_summary, 1, s_size_cache, f);
    fclose(f);
    s_summary[n] = '\0';
}

void memory_store_tick(void)
{
    if (!s_ready || !s_writes_safe) return;
    if (s_pending) {
        if (!tts_client_is_playing()) {   /* TTS 空闲才冲刷 */
            memory_store_write_now(s_pending);
            free(s_pending);
            s_pending = NULL;
        }
    }
    /* 刷新元数据缓存 — 发送路径 (ws_client_send_chat) 不直接访问 FatFS */
    memory_store_refresh_cache();
}

size_t memory_store_cached_size(void) { return s_size_valid ? s_size_cache : 0; }
const char *memory_store_cached_summary(void) { return s_summary; }
