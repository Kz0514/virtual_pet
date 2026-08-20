/**
 * @file memory_store.c
 * @brief 设备端对话记忆 — /cfg 分区 memory.txt (LittleFS 内部存储)
 *
 * 完整对话历史存设备端 (上限 100KB), 超限由服务端 LLM 按
 * 信息重要性 + 时间远近压缩后下发覆盖 (memory_update 消息)。
 *
 * /cfg 由 sensor_logger_init 挂载 (LittleFS, 掉电安全),
 * 本模块只负责 memory.txt 的读写。
 * 写盘纪律: 一切 flash 访问 (读与写) 统一由专用写盘任务执行 — 调用方
 * 可能是 PSRAM 栈任务 (esp_websocket_client 任务栈在 PSRAM), 而 flash
 * 操作期间 cache 被禁用, PSRAM 栈一访问即 double exception (聊天回复
 * 崩溃根因: 1.0.213 只移了写, 1.0.214 证实 stat/fopen 读同样崩)。
 * 另: TTS 播放期间不写盘, 防 flash 冻结卡音频。
 */
#include "memory_store.h"
#include "time_manager.h"
#include "tts_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>

static const char *TAG = "memory";
static const char *MEM_FILE  = "/cfg/memory.txt";

#define MEM_MAX_BYTES  (100 * 1024)
#define SUMMARY_MAX    (4 * 1024)   /* ≤4KB 视为"压缩态", 随 chat 消息携带 */

static bool  s_ready   = false;
static bool  s_writes_safe = true;  /* 低电量闸: 电池 <3.7V 暂停写盘 */

void memory_store_set_writes_safe(bool safe) { s_writes_safe = safe; }
bool memory_store_writes_safe(void) { return s_writes_safe; }

/* 元数据缓存 — 主循环 tick 刷新, 发送路径零 FatFS 访问 */
static char  *s_summary = NULL;     /* PSRAM, ≤4KB 压缩态摘要 */
static size_t s_size_cache = 0;
static bool   s_size_valid = false;

static void memory_store_refresh_cache(void);

/* ===== 异步读写 — 专用写盘任务 (内部 RAM 栈) =====
 * WS 任务栈在 PSRAM (vendored esp_websocket_client 补丁); flash 操作
 * (读写都一样) 期间 cache 禁用 → PSRAM 栈一访问即 double exception
 * (1.0.213 修了写, 1.0.214 补上读 — 读也走 cache 禁用路径)。
 * 所有读写一律入队, 由本任务执行 (xTaskCreate 默认内部 RAM 栈,
 * init 在启动初期创建, 内部堆未碎片化可稳定分配)。 */
typedef enum { MEM_WRITE_APPEND, MEM_WRITE_OVERWRITE, MEM_READ } mem_write_op_t;

typedef struct {
    mem_write_op_t op;
    char *user;       /* APPEND: 可为 NULL */
    char *assistant;  /* APPEND: 可为 NULL */
    char *content;    /* OVERWRITE */
    memory_read_cb_t cb;  /* READ: 结果回调 (写盘任务上下文执行) */
    void *arg;            /* READ: 回调透传参数 (回调返回后由调用方释放) */
} mem_write_item_t;

#define MEM_WRITE_Q_LEN  4
#define MEM_WRITE_STACK  (6 * 1024)

static QueueHandle_t s_write_q = NULL;

static void memory_store_writer_task(void *arg)
{
    (void) arg;
    mem_write_item_t it;
    while (xQueueReceive(s_write_q, &it, portMAX_DELAY) == pdTRUE) {
        /* TTS 播放期间写盘冻结双核 100-400ms → 音频卡顿, 等空闲再写 (上限 30s) */
        int waited = 0;
        while (tts_client_is_playing() && waited < 30000) {
            vTaskDelay(pdMS_TO_TICKS(200));
            waited += 200;
        }
        if (it.op == MEM_WRITE_OVERWRITE) {
            FILE *f = fopen(MEM_FILE, "w");
            if (f) {
                fwrite(it.content, 1, strlen(it.content), f);
                fclose(f);
                ESP_LOGI(TAG, "记忆已覆盖 (%d B)", (int)strlen(it.content));
            } else {
                ESP_LOGW(TAG, "打开记忆文件失败");
            }
        } else if (it.op == MEM_WRITE_APPEND) {
            FILE *f = fopen(MEM_FILE, "a");
            if (f) {
                /* 时间戳在写入时刻生成: NTP 同步后行首 [MM-DD HH:MM], 未同步
                 * 不带前缀 (兼容旧格式, 服务端压缩/LLM 可理解混合) */
                char ts[24] = "";   /* 留足空间防 format-truncation */
                if (time_manager_is_synced()) {
                    time_t t = (time_t)time_manager_get_unix_sec();
                    struct tm tm;
                    localtime_r(&t, &tm);
                    snprintf(ts, sizeof(ts), "[%02d-%02d %02d:%02d] ",
                             tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
                }
                if (it.user && it.user[0])
                    fprintf(f, "%s主人: %s\n", ts, it.user);
                if (it.assistant && it.assistant[0])
                    fprintf(f, "%s萝莉丝: %s\n", ts, it.assistant);
                fclose(f);
            } else {
                ESP_LOGW(TAG, "打开记忆文件失败");
            }
        } else if (it.op == MEM_READ) {
            /* 读也在这里执行 — PSRAM 栈任务 (WS) 上 stat/fopen 同样是
             * cache 禁用期 flash 访问, 必崩 (1.0.214 根因) */
            char *buf = NULL;
            size_t len = 0;
            if (s_ready) {
                len = memory_store_size();
                if (len > 0) {
                    buf = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
                    if (buf) {
                        FILE *f = fopen(MEM_FILE, "r");
                        if (f) {
                            size_t n = fread(buf, 1, len, f);
                            fclose(f);
                            buf[n] = '\0';
                        } else {
                            free(buf);
                            buf = NULL;
                            len = 0;
                        }
                    } else {
                        len = 0;   /* PSRAM 不足 → 回传空, 服务端按无记忆处理 */
                    }
                }
            }
            if (it.cb) it.cb(buf, len, it.arg);   /* 回调返回后缓冲即失效 */
            free(buf);
        }
        free(it.user);
        free(it.assistant);
        free(it.content);
    }
}

/* 入队 — 立即拷贝, 调用方字符串生命周期与写盘解耦; 队列满则阻塞至多 2s */
static esp_err_t memory_store_enqueue(mem_write_op_t op, const char *user,
                                      const char *assistant, const char *content)
{
    if (!s_write_q) return ESP_FAIL;
    mem_write_item_t it = { .op = op, .user = NULL, .assistant = NULL, .content = NULL };
    if (user) { it.user = strdup(user); if (!it.user) return ESP_ERR_NO_MEM; }
    if (assistant) {
        it.assistant = strdup(assistant);
        if (!it.assistant) { free(it.user); return ESP_ERR_NO_MEM; }
    }
    if (content) {
        it.content = strdup(content);
        if (!it.content) { free(it.user); free(it.assistant); return ESP_ERR_NO_MEM; }
    }
    if (xQueueSend(s_write_q, &it, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "记忆写队列满 — 本次写入丢弃");
        free(it.user); free(it.assistant); free(it.content);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

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
    s_write_q = xQueueCreate(MEM_WRITE_Q_LEN, sizeof(mem_write_item_t));
    if (!s_write_q) {
        ESP_LOGE(TAG, "写盘队列创建失败 — 记忆写入不可用");
        s_ready = false;
        return ESP_ERR_NO_MEM;
    }
    /* 写盘任务: 栈必须内部 RAM — PSRAM 栈上写 flash 会 double exception */
    if (xTaskCreatePinnedToCore(memory_store_writer_task, "mem_writer", MEM_WRITE_STACK,
                                NULL, 5, NULL, tskNO_AFFINITY) != pdTRUE) {
        ESP_LOGE(TAG, "写盘任务创建失败 — 记忆写入不可用");
        vQueueDelete(s_write_q);
        s_write_q = NULL;
        s_ready = false;
        return ESP_ERR_NO_MEM;
    }
    memory_store_refresh_cache();
    ESP_LOGI(TAG, "记忆文件就绪, memory.txt %u B (写盘任务已启)", (unsigned)memory_store_size());
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
    /* 用 tick 刷新的缓存值判超限 — stat() 也是 flash 读, PSRAM 栈任务上会崩
     * (1.0.213 只修了写, 此处残留的 stat 就是 1.0.214 崩点) */
    if (memory_store_cached_size() > MEM_MAX_BYTES)
        ESP_LOGW(TAG, "记忆已超 100KB, 等待服务端压缩");
    return memory_store_enqueue(MEM_WRITE_APPEND, user, assistant, NULL);
}

esp_err_t memory_store_read_async(memory_read_cb_t cb, void *arg)
{
    if (!cb || !s_write_q) return ESP_FAIL;
    mem_write_item_t it = { .op = MEM_READ, .cb = cb, .arg = arg };
    if (xQueueSend(s_write_q, &it, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "记忆读队列满 — 读取请求丢弃");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t memory_store_overwrite(const char *content)
{
    if (!s_ready || !content) return ESP_FAIL;
    return memory_store_enqueue(MEM_WRITE_OVERWRITE, NULL, NULL, content);
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
    /* 刷新元数据缓存 — 发送路径 (ws_client_send_chat) 不直接访问 FatFS;
     * 写盘由专用任务消化队列, 无需在此冲刷 */
    memory_store_refresh_cache();
}

size_t memory_store_cached_size(void) { return s_size_valid ? s_size_cache : 0; }
const char *memory_store_cached_summary(void) { return s_summary; }
