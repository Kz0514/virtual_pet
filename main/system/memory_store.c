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
 * 操作期间 cache 被禁用, PSRAM 栈一访问即 double exception。
 * 另: TTS 播放期间不写盘, 防 flash 冻结卡音频。
 */
#include "memory_store.h"
#include "time_manager.h"
#include "tts_client.h"
#include "config_mgr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

static const char *TAG = "memory";
static const char *MEM_FILE = "/cfg/memory.txt";

#define MEM_MAX_BYTES (100 * 1024)
#define SUMMARY_MAX (4 * 1024) /* ≤4KB 视为"压缩态", 随 chat 消息携带 */
#define MEM_MAX_FILE (256 * 1024) /* 设计上限 100KB, >256KB = LittleFS 元数据损坏 */
#define MEM_REPAIR_MAX (128 * 1024) /* 自愈读全文件上限 */

/* : UTF-8 清洗 — 坏字节就地替换为 '?' (长度不变, 返回坏字节数,
 * 输出仍合法 UTF-8)。坏点源于 ws_event 的 512B snprintf 行尾截断;
 * '?' 独立占位, 其余字节解析不受影响, LLM/日志可感知缺失。坏字节
 * 随 mem_summary 上送会被服务端 decoder 拒连, 必须清洗。 */
static size_t utf8_sanitize_inplace(char *s, size_t len)
{
    size_t i = 0, bad = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        size_t need;
        if (c < 0x80) {
            i++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF)
            need = 2; /* 2 字节 */
        else if (c >= 0xE0 && c <= 0xEF)
            need = 3; /* 3 字节 */
        else if (c >= 0xF0 && c <= 0xF4)
            need = 4; /* 4 字节 */
        else {
            s[i] = '?';
            bad++;
            i++;
            continue;
        }                               /* 孤立 continuation/非法起始 */
        bool is_bad = (i + need > len); /* 字符被截断 */
        for (size_t k = 1; !is_bad && k < need; k++) {
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) is_bad = true; /* continuation 非法 */
        }
        if (is_bad) {
            s[i] = '?';
            bad++;
            i++;
            continue;
        } /* 坏起始字节 — 占位, 后续重新解析 */
        i += need;
    }
    return bad;
}

static bool s_ready = false;
static bool s_writes_safe = true; /* 低电量闸: 电池 <3.7V 暂停写盘 */
static bool s_repair_pending = false; /* 自愈去重: 一个周期只入队一次 */

void memory_store_set_writes_safe(bool safe) { s_writes_safe = safe; }
bool memory_store_writes_safe(void) { return s_writes_safe; }

/* 元数据缓存 — 主循环 tick 刷新, 发送路径零 FatFS 访问 */
static char *s_summary = NULL; /* PSRAM, ≤4KB 压缩态摘要 */
static size_t s_size_cache = 0;
static bool s_size_valid = false;

static void memory_store_refresh_cache(void);

/* ===== 异步读写 — 专用写盘任务 (内部 RAM 栈) =====
 * WS 任务栈在 PSRAM (vendored esp_websocket_client 补丁); flash 操作
 * (读写都一样) 期间 cache 禁用 → PSRAM 栈一访问即 double exception。
 * 所有读写一律入队, 由本任务执行 (xTaskCreate 默认内部 RAM 栈,
 * init 在启动初期创建, 内部堆未碎片化可稳定分配)。 */
typedef enum { MEM_WRITE_APPEND,
               MEM_WRITE_OVERWRITE,
               MEM_READ,
               MEM_WRITE_REPAIR } mem_write_op_t;

typedef struct {
    mem_write_op_t op;
    char *user;          /* APPEND: 可为 NULL */
    char *assistant;     /* APPEND: 可为 NULL */
    char *content;       /* OVERWRITE */
    memory_read_cb_t cb; /* READ: 结果回调 (写盘任务上下文执行) */
    void *arg;           /* READ: 回调透传参数 (回调返回后由调用方释放) */
} mem_write_item_t;

#define MEM_WRITE_Q_LEN 4
#define MEM_WRITE_STACK (6 * 1024)

static QueueHandle_t s_write_q = NULL;

static void memory_store_writer_task(void *arg)
{
    (void)arg;
    mem_write_item_t it;
    while (xQueueReceive(s_write_q, &it, portMAX_DELAY) == pdTRUE) {
        /* TTS 播放期间写盘冻结双核 100-400ms → 音频卡顿, 等空闲再写 (上限 30s) */
        int waited = 0;
        while (tts_client_is_playing() && waited < 30000) {
            vTaskDelay(pdMS_TO_TICKS(200));
            waited += 200;
        }
        if (it.op == MEM_WRITE_OVERWRITE) {
            /* : fopen → open (newlib FILE+锁 内部 RAM 分配失败会 abort) */
            int fd = open(MEM_FILE, O_CREAT | O_TRUNC | O_WRONLY);
            if (fd >= 0) {
                /* : 写入前清洗 — 坏字节替换 '?' (strdup 副本可改), 全坏不写 */
                size_t cl = strlen(it.content);
                size_t bad = utf8_sanitize_inplace(it.content, cl);
                if (bad < cl) write(fd, it.content, cl);
                close(fd);
                ESP_LOGI(TAG, "记忆已覆盖 (%u B)", (unsigned)cl);
            } else {
                ESP_LOGW(TAG, "打开记忆文件失败");
            }
        } else if (it.op == MEM_WRITE_APPEND) {
            int fd = open(MEM_FILE, O_CREAT | O_APPEND | O_WRONLY);
            if (fd >= 0) {
                /* 时间戳在写入时刻生成: NTP 同步后行首 [MM-DD HH:MM], 未同步
                 * 不带前缀 (兼容旧格式, 服务端压缩/LLM 可理解混合) */
                char ts[24] = ""; /* 留足空间防 format-truncation */
                if (time_manager_is_synced()) {
                    time_t t = (time_t)time_manager_get_unix_sec();
                    struct tm tm;
                    localtime_r(&t, &tm);
                    snprintf(ts, sizeof(ts), "[%02d-%02d %02d:%02d] ",
                             tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
                }
                /* 分段 write (O_APPEND 每次自动到末尾, 单写盘任务无竞争)。
                 * 每段清洗 — 坏字节替换 '?', 全坏行不写 */
                if (it.user) {
                    size_t ul = strlen(it.user);
                    size_t ubad = utf8_sanitize_inplace(it.user, ul);
                    if (ubad < ul) {
                        write(fd, ts, strlen(ts));
                        char prefix[CFG_STR_MAX + 2];
                        int pl = snprintf(prefix, sizeof(prefix), "%s: ",
                                          config_get_str("owner_name", "主人"));
                        write(fd, prefix, pl);
                        write(fd, it.user, ul);
                        write(fd, "\n", 1);
                    }
                }
                if (it.assistant) {
                    size_t al = strlen(it.assistant);
                    size_t abad = utf8_sanitize_inplace(it.assistant, al);
                    if (abad < al) {
                        write(fd, ts, strlen(ts));
                        char prefix[CFG_STR_MAX + 2];
                        int pl = snprintf(prefix, sizeof(prefix), "%s: ",
                                          config_get_str("pet_name", "萝莉丝"));
                        write(fd, prefix, pl);
                        write(fd, it.assistant, al);
                        write(fd, "\n", 1);
                    }
                }
                close(fd);
            } else {
                ESP_LOGW(TAG, "打开记忆文件失败");
            }
        } else if (it.op == MEM_READ) {
            /* 读也在这里执行 — PSRAM 栈任务 (WS) 上 stat/fopen 同样是
             * cache 禁用期 flash 访问, 必崩 */
            char *buf = NULL;
            size_t len = 0;
            if (s_ready) {
                len = memory_store_size();
                if (len > 0) {
                    buf = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
                    if (buf) {
                        int fd = open(MEM_FILE, O_RDONLY);
                        if (fd >= 0) {
                            ssize_t n = read(fd, buf, len);
                            close(fd);
                            if (n < 0) n = 0;
                            buf[n] = '\0';
                        } else {
                            free(buf);
                            buf = NULL;
                            len = 0;
                        }
                    } else {
                        len = 0; /* PSRAM 不足 → 回传空, 服务端按无记忆处理 */
                    }
                }
            }
            if (it.cb) it.cb(buf, len, it.arg); /* 回调返回后缓冲即失效 */
            free(buf);
        } else if (it.op == MEM_WRITE_REPAIR) {
            /* 自愈: 读全文件 → 清洗坏字节 → O_TRUNC 原子重写。
             * 全在本任务内执行 → 与 APPEND/OVERWRITE 严格串行 (修复写回
             * 必须入队单写者, 写回用实际读取长度, 缓冲已补终止)。
             * stat 尺寸异常 (崩溃期脏 LittleFS 元数据垃圾尺寸,
             * memory_store_size 已带 256KB 上限 → 0) 时按
             * MEM_REPAIR_MAX 尝试读实际数据, 读 0 字节则不动 (空文件) */
            size_t flen = memory_store_size();
            if (flen == 0 || flen > MEM_REPAIR_MAX) flen = MEM_REPAIR_MAX;
            char *fbuf = heap_caps_malloc(flen + 1, MALLOC_CAP_SPIRAM);
            if (fbuf) {
                int fd = open(MEM_FILE, O_RDONLY);
                if (fd >= 0) {
                    size_t got = 0;
                    while (got < flen) {
                        ssize_t r = read(fd, fbuf + got, flen - got);
                        if (r <= 0) break;
                        got += (size_t)r;
                    }
                    close(fd);
                    if (got > 0) {
                        fbuf[got] = '\0';
                        size_t bad = utf8_sanitize_inplace(fbuf, got);
                        if (bad > 0) {
                            int wfd = open(MEM_FILE, O_CREAT | O_TRUNC | O_WRONLY);
                            if (wfd >= 0) {
                                size_t total = strlen(fbuf);
                                if (total > 0 &&
                                    write(wfd, fbuf, total) != (ssize_t)total)
                                    ESP_LOGW(TAG, "记忆文件修复写入失败");
                                close(wfd);
                                ESP_LOGI(TAG, "记忆文件修复完成 (%u 坏字节 → '?', %u B)",
                                         (unsigned)bad, (unsigned)total);
                            }
                        }
                    }
                }
                free(fbuf);
            }
            s_repair_pending = false; /* 允许下周期重新入队 */
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
    mem_write_item_t it = {.op = op, .user = NULL, .assistant = NULL, .content = NULL};
    if (user) {
        it.user = strdup(user);
        if (!it.user) return ESP_ERR_NO_MEM;
    }
    if (assistant) {
        it.assistant = strdup(assistant);
        if (!it.assistant) {
            free(it.user);
            return ESP_ERR_NO_MEM;
        }
    }
    if (content) {
        it.content = strdup(content);
        if (!it.content) {
            free(it.user);
            free(it.assistant);
            return ESP_ERR_NO_MEM;
        }
    }
    if (xQueueSend(s_write_q, &it, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "记忆写队列满 — 本次写入丢弃");
        free(it.user);
        free(it.assistant);
        free(it.content);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t memory_store_init(void)
{
    /* /data 由 sensor_logger_init 挂载 (main.c 中先于本模块 init) */
    int fd = open(MEM_FILE, O_CREAT | O_APPEND | O_WRONLY); /* 存在则保持内容, 不存在则创建 */
    if (fd < 0) {
        ESP_LOGW(TAG, "data 分区不可用 — 请确认 sensor_logger_init 已先执行");
        return ESP_FAIL;
    }
    close(fd);
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
    /* : 崩溃期脏 LittleFS 元数据可给垃圾尺寸; 设计上限 100KB, >256KB
     * 即损坏, 返回 0 防垃圾尺寸传播 (s_size_cache/读缓冲分配/超限判断) */
    if (st.st_size > (off_t)MEM_MAX_FILE) return 0;
    return (size_t)st.st_size;
}

esp_err_t memory_store_append(const char *user, const char *assistant)
{
    if (!s_ready || !s_writes_safe || (!user && !assistant)) return ESP_FAIL;
    /* 用 tick 刷新的缓存值判超限 — stat 也是 flash 读, PSRAM 栈任务上会崩 */
    if (memory_store_cached_size() > MEM_MAX_BYTES) /* 勿漏 () — 函数指针与整数
            比较恒真, 会误报"记忆已超 100KB" */
        ESP_LOGW(TAG, "记忆已超 100KB, 等待服务端压缩");
    return memory_store_enqueue(MEM_WRITE_APPEND, user, assistant, NULL);
}

esp_err_t memory_store_read_async(memory_read_cb_t cb, void *arg)
{
    if (!cb || !s_write_q) return ESP_FAIL;
    mem_write_item_t it = {.op = MEM_READ, .cb = cb, .arg = arg};
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
    if (s_summary) {
        free(s_summary);
        s_summary = NULL;
    }
    if (s_size_cache == 0 || s_size_cache > SUMMARY_MAX) return; /* 非压缩态 */
    s_summary = heap_caps_malloc(s_size_cache + 1, MALLOC_CAP_SPIRAM);
    if (!s_summary) return;
    int fd = open(MEM_FILE, O_RDONLY);
    if (fd < 0) {
        free(s_summary);
        s_summary = NULL;
        return;
    }
    ssize_t n = read(fd, s_summary, s_size_cache);
    close(fd);
    if (n < 0) n = 0;
    s_summary[n] = '\0'; /* : 缓冲必补终止 — 否则 strlen/写回越读附尾 */
    /* : UTF-8 校验 — 历史文件可能残留截半字符 (512B snprintf 截断 +
     * 崩溃期脏写), 坏字节随 mem_summary 上送会被服务端拒连。
     * 发现即入队自愈修复 (MEM_WRITE_REPAIR 由写盘任务执行 — O_TRUNC
     * 重写与 append 严格串行)。占位保留记录结构, 服务端 LLM 压缩可整合。 */
    size_t bad = utf8_sanitize_inplace(s_summary, (size_t)n);
    if (bad > 0) {
        ESP_LOGW(TAG, "记忆文件含无效 UTF-8 (%u 坏字节) — 已入队修复",
                 (unsigned)bad);
        if (!s_repair_pending) {
            if (memory_store_enqueue(MEM_WRITE_REPAIR, NULL, NULL, NULL) == ESP_OK)
                s_repair_pending = true;
            else
                ESP_LOGW(TAG, "记忆修复入队失败 — 下个周期重试");
        }
    }
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
