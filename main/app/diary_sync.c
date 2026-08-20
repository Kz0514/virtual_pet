/**
 * @file diary_sync.c
 * @brief 日记 HTML 同步 — 服务端渲染单文件页 → /data/diary/YYYY-MM-DD.html
 *
 * 数据流: 服务端 /api/v1/diary/list?month=当月 → 每条缺文件 →
 *         GET /diary/{id}/html (完整信纸页, 涂鸦 base64 内嵌) → tmp+rename 落盘。
 * 触发: 首次连接后 + 每 6h (节拍在 main.c 2s tick, 只判条件发通知,
 *       HTTP+写盘全部在独立任务, 不阻塞 UI 主循环)。
 * 已存在跳过 — 用户在 USB 模式删掉某篇即强制刷新 (与 USB 读写联动)。
 * 配额: 30 文件 / 512KB, 超限按文件名 (日期) 删最旧。
 * 闸门: 低电量 (writes_safe) 与 TTS 播放期间不写 — 与 life_log 同策略。
 */
#include "diary_sync.h"
#include "api_client.h"
#include "server_config.h"
#include "time_manager.h"
#include "wifi_manager.h"
#include "sensor_logger.h"
#include "memory_store.h"
#include "tts_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

static const char *TAG = "diary_sync";

#define SYNC_INTERVAL_S     (6 * 3600)     /* 同步节流 6h (成功后) */
#define RETRY_INTERVAL_S    (10 * 60)      /* 失败重试 10min (服务端未就绪/断网) */
#define LIST_CAP            (64 * 1024)    /* 列表 JSON 上限 (31 篇 × ~700B) */
#define HTML_CAP            (192 * 1024)   /* 单篇 HTML 上限 (含涂鸦 b64) */
#define HTTP_TIMEOUT_MS     10000
#define DIARY_DIR           "/data/diary"
#define MAX_FILES           30
#define MAX_TOTAL_BYTES     (512 * 1024)

static TaskHandle_t s_task = NULL;
static bool         s_busy = false;        /* 任务执行中 (tick 不再触发) */
static bool         s_first_done = false;
static uint32_t     s_next_run = 0;        /* unix 秒 */

/* ── HTTP GET → PSRAM 动态缓冲 (响应可至 ~200KB) ── */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} get_ctx_t;

static esp_err_t get_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    get_ctx_t *ctx = evt->user_data;
    size_t room = ctx->cap - ctx->len - 1;
    size_t n = evt->data_len < room ? evt->data_len : room;
    memcpy(ctx->buf + ctx->len, evt->data, n);
    ctx->len += n;
    return ESP_OK;
}

static char *http_get(const char *url, size_t cap)
{
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) { ESP_LOGE(TAG, "缓冲分配失败 (%uB)", (unsigned)cap); return NULL; }
    get_ctx_t ctx = { buf, cap, 0 };

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = get_handler,
        .user_data = &ctx,
        .timeout_ms = HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(buf); return NULL; }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "GET 失败: err=%d status=%d", err, status);
        free(buf);
        return NULL;
    }
    buf[ctx.len] = '\0';
    return buf;
}

/* ── 配额: 文件数/总大小超限 → 按文件名删最旧 (YYYY-MM-DD.html 字典序=时间序) ── */
static void enforce_quota(void)
{
    DIR *d = opendir(DIARY_DIR);
    if (!d) return;
    int count = 0;
    size_t total = 0;
    char oldest[288] = {0};
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;             /* README 之外的点文件 */
        size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 5, ".html") != 0) continue;
        count++;
        struct stat st;
        char p[320];
        snprintf(p, sizeof(p), "%s/%s", DIARY_DIR, e->d_name);
        if (stat(p, &st) == 0) total += (size_t)st.st_size;
        if (oldest[0] == 0 || strcmp(e->d_name, oldest) < 0)
            snprintf(oldest, sizeof(oldest), "%s", e->d_name);
    }
    closedir(d);

    /* 只删 .html; README 不在统计内且不受影响 */
    while ((count > MAX_FILES || total > MAX_TOTAL_BYTES) && oldest[0]) {
        char p[320];
        snprintf(p, sizeof(p), "%s/%s", DIARY_DIR, oldest);
        struct stat st;
        if (stat(p, &st) == 0) total -= (size_t)st.st_size;
        if (remove(p) == 0)
            ESP_LOGI(TAG, "配额裁剪: 删除 %s", p);
        count--;
        /* 重新找最旧 */
        oldest[0] = 0;
        d = opendir(DIARY_DIR);
        if (!d) break;
        while ((e = readdir(d)) != NULL) {
            size_t n = strlen(e->d_name);
            if (e->d_name[0] == '.' || n < 5 ||
                strcmp(e->d_name + n - 5, ".html") != 0) continue;
            if (oldest[0] == 0 || strcmp(e->d_name, oldest) < 0)
                snprintf(oldest, sizeof(oldest), "%s", e->d_name);
        }
        closedir(d);
    }
}

/* TTS 播放期间等待 — 与 life_log 同策略 (flash 写会冻结双核卡音频) */
static void wait_tts_idle(void)
{
    while (tts_client_is_playing())
        vTaskDelay(pdMS_TO_TICKS(100));
}

/* 返回 true = 服务端列表拉取成功 (此后按 6h 节流); false = 未就绪 (10min 重试) */
static bool run_sync(void)
{
    if (!api_client_is_authenticated() || !sensor_logger_data_mounted()) return false;

    /* 当月 YYYY-MM (本地时区) */
    time_t t = (time_t)time_manager_get_unix_sec();
    struct tm tm;
    localtime_r(&t, &tm);
    char month[32];
    snprintf(month, sizeof(month), "%04d-%02d",
             (int)tm.tm_year + 1900, (int)tm.tm_mon + 1);

    /* 1. 拉当月列表 */
    char url[640];
    snprintf(url, sizeof(url), "http://%s:%d/api/v1/diary/list?month=%s&token=%s",
             SERVER_HOST, SERVER_PORT, month, api_client_get_token());
    char *list_json = http_get(url, LIST_CAP);
    if (!list_json) return false;

    cJSON *root = cJSON_Parse(list_json);
    free(list_json);
    if (!root) { ESP_LOGW(TAG, "列表 JSON 解析失败"); return false; }

    cJSON *entries = cJSON_GetObjectItem(root, "entries");
    if (!cJSON_IsArray(entries)) { cJSON_Delete(root); return false; }

    int n = cJSON_GetArraySize(entries);
    ESP_LOGI(TAG, "本月 %d 篇日记", n);

    /* 先确认目录 (可能被 USB 用户删除) */
    struct stat st;
    if (stat(DIARY_DIR, &st) != 0 || !S_ISDIR(st.st_mode))
        mkdir(DIARY_DIR, 0777);

    cJSON *it;
    int synced = 0;
    cJSON_ArrayForEach(it, entries) {
        cJSON *id     = cJSON_GetObjectItem(it, "id");
        cJSON *edate  = cJSON_GetObjectItem(it, "entry_date");
        if (!cJSON_IsString(id) || !cJSON_IsString(edate)) continue;

        /* 文件名 = 日期; 已存在跳过 (USB 删文件可强制刷新) */
        char path[64], tmp[72];
        snprintf(path, sizeof(path), "%s/%s.html", DIARY_DIR, edate->valuestring);
        if (access(path, F_OK) == 0) continue;

        char html_url[768];
        snprintf(html_url, sizeof(html_url),
                 "http://%s:%d/api/v1/diary/%s/html?token=%s",
                 SERVER_HOST, SERVER_PORT, id->valuestring,
                 api_client_get_token());
        char *html = http_get(html_url, HTML_CAP);
        if (!html) continue;   /* 单篇失败不阻断整体 */

        wait_tts_idle();
        if (!memory_store_writes_safe()) { free(html); continue; }

        /* tmp+rename 原子写 — 断电不产生半个文件 */
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        FILE *f = fopen(tmp, "wb");
        if (f) {
            size_t len = strlen(html);
            if (fwrite(html, 1, len, f) == len) {
                fclose(f);
                if (rename(tmp, path) == 0) {
                    ESP_LOGI(TAG, "已同步 %s (%u B)", path, (unsigned)len);
                    synced++;
                } else {
                    remove(tmp);
                }
            } else {
                fclose(f);
                remove(tmp);
            }
        }
        free(html);
    }
    cJSON_Delete(root);

    enforce_quota();
    ESP_LOGI(TAG, "同步完成: 新 %d 篇 (共 %d)", synced, n);
    return true;   /* 列表拉取成功 → 按 6h 节流; 失败才 10min 重试 */
}

static void diary_sync_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        bool ok = run_sync();
        s_busy = false;
        s_first_done = true;
        s_next_run = (uint32_t)time_manager_get_unix_sec()
                     + (ok ? SYNC_INTERVAL_S : RETRY_INTERVAL_S);
    }
}

void diary_sync_tick(void)
{
    if (s_busy || !s_task) return;
    if (!time_manager_is_synced()) return;
    if (s_first_done &&
        (uint32_t)time_manager_get_unix_sec() < s_next_run) return;
    /* 前置条件: 联网 + 认证 + /data 可用 + 低电量闸 + TTS 闸 */
    if (!wifi_is_connected() || !api_client_is_authenticated() ||
        !sensor_logger_data_mounted() || !memory_store_writes_safe() ||
        tts_client_is_playing()) return;

    s_busy = true;
    xTaskNotifyGive(s_task);
}

esp_err_t diary_sync_init(void)
{
    /* 栈保持内部 RAM — 本任务直接写 /data (flash 写期间 cache 冻结,
     * PSRAM 栈会 double exception, memory_store 实测) */
    if (xTaskCreate(diary_sync_task, "diary_sync", 8192,
                    NULL, 1, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "就绪 (间隔 %uh, 目录 %s)", SYNC_INTERVAL_S / 3600, DIARY_DIR);
    return ESP_OK;
}
