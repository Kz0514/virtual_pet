/**
 * @file life_log.c
 * @brief 全量交互日志 — 记录每一次互动 (对话/摸头/摇晃/语音), 带时间戳
 *
 * 文件: /data/life/log.txt (FatFS 用户可见分区, USB 直读展示)。
 * 注意: LLM 上下文**不**从这里取 (仍只读 /cfg/memory.txt) —
 * 这份文件是给主人看的"生命记录"。
 *
 * 实现: 入队-落盘分离 — 调用方 (LVGL 定时器/WS 事件回调等小栈上下文)
 * 只 malloc + 入队, 由专用任务 (4KB 栈) 统一写盘, 天然串行化;
 * TTS 播放期间任务内等待, 日志不丢且音频不被 flash 冻结卡顿。
 * 512KB 滚动: 超限轮转为 log.old, 再超限丢弃 log.old。
 */
#include "life_log.h"
#include "sensor_logger.h"
#include "time_manager.h"
#include "tts_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static const char *TAG = "life_log";

#define LIFE_DIR        "/data/life"
#define LIFE_FILE       "/data/life/log.txt"
#define LIFE_OLD_FILE   "/data/life/log.old"
#define LIFE_MAX_BYTES  (512 * 1024)
#define LIFE_QUEUE_LEN  16
#define LIFE_LINE_MAX   256

static QueueHandle_t s_q = NULL;

/* 落盘 (专用任务上下文, 4KB 栈) */
static void append_line(const char *ts, const char *line)
{
    if (!sensor_logger_data_mounted()) return;

    /* 目录自愈 (幂等): 首启 / 格式化后 /data/life 不存在 → 重建 */
    mkdir(LIFE_DIR, 0777);

    /* 512KB 滚动: 超限 → log.old (已存在则丢弃), 再超限丢 log.old */
    struct stat st;
    if (stat(LIFE_FILE, &st) == 0 && st.st_size > LIFE_MAX_BYTES) {
        remove(LIFE_OLD_FILE);
        rename(LIFE_FILE, LIFE_OLD_FILE);
    }

    FILE *f = fopen(LIFE_FILE, "a");
    if (!f) return;
    fprintf(f, "[%s] %s\n", ts, line);
    fclose(f);
}

static void life_log_task(void *arg)
{
    (void)arg;
    char line[LIFE_LINE_MAX];
    for (;;) {
        char *msg = NULL;
        if (xQueueReceive(s_q, &msg, portMAX_DELAY) != pdTRUE || !msg) continue;
        strncpy(line, msg, LIFE_LINE_MAX - 1);
        line[LIFE_LINE_MAX - 1] = '\0';
        free(msg);

        /* TTS 播放期间等待 — flash 写会冻结双核卡音频; 日志可等, 音频不可卡 */
        while (tts_client_is_playing())
            vTaskDelay(pdMS_TO_TICKS(100));

        char ts[32];
        if (time_manager_is_synced()) {
            time_t t = (time_t)time_manager_get_unix_sec();
            struct tm tm;
            localtime_r(&t, &tm);
            snprintf(ts, sizeof(ts), "%02d-%02d %02d:%02d",
                     tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
        } else {
            snprintf(ts, sizeof(ts), "--:-- --:--");   /* 未校时 */
        }
        append_line(ts, line);
    }
}

void life_log_line(const char *fmt, ...)
{
    if (!s_q || !sensor_logger_data_mounted()) return;

    char *buf = heap_caps_malloc(LIFE_LINE_MAX, MALLOC_CAP_SPIRAM);
    if (!buf) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, LIFE_LINE_MAX, fmt, ap);
    va_end(ap);
    if (xQueueSend(s_q, &buf, 0) != pdTRUE)
        free(buf);   /* 队列满 → 丢最新, 日志可丢 */
}

esp_err_t life_log_init(void)
{
    s_q = xQueueCreate(LIFE_QUEUE_LEN, sizeof(char *));
    if (!s_q) return ESP_ERR_NO_MEM;
    TaskHandle_t th;
    /* 栈保持内部 RAM — 本任务直接写 /data (flash 写期间 cache 冻结,
     * PSRAM 栈 double exception, memory_store 实测) */
    if (xTaskCreatePinnedToCore(life_log_task, "life_log", 4096,
                                NULL, 2, &th, 0) != pdPASS) {
        vQueueDelete(s_q);
        s_q = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "就绪 (%s)", LIFE_FILE);
    return ESP_OK;
}
