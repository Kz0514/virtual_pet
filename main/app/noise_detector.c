/** @file noise_detector.c @brief 噪音检测 + 级联累加器 → CSV */
#include "noise_detector.h"
#include "es8311_drv.h"
#include "voice_chat.h"
#include "tts_client.h"
#include "session_mgr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sys/time.h"
#include "sys/stat.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "noise";

#define SAMPLE_DURATION_MS  100
#define SAMPLE_INTERVAL_MS  2000
#define SAMPLE_RATE          48000
#define FRAME_MS             20
#define FRAME_SAMPLES       (SAMPLE_RATE * FRAME_MS / 1000)   /* 960 */
#define TOTAL_FRAMES        (SAMPLE_DURATION_MS / FRAME_MS)   /* 5 */

/* ── 层级累加器 ── */
typedef struct {
    uint32_t sum;
    uint16_t count;
} bucket_t;

/* 每层窗口宽度 (秒) */
static const uint32_t WIN_SECS[NOISE_WIN_COUNT] = {
    5, 10, 30, 60, 180, 600, 1200, 3600, 10800, 28800, 86400, 259200
};

static bucket_t   s_buckets[NOISE_WIN_COUNT];
static uint8_t    s_last_avg[NOISE_WIN_COUNT];  /* 最近一次完成值 */
static uint8_t    s_raw_level;
static uint32_t   s_bucket_t0[NOISE_WIN_COUNT]; /* 每个桶的开始时间戳 */
static bool       s_ready = false;

/* ── CSV 文件路径 ── */
#define NOISE_CSV_PATH  "/cfg/noise.csv"

static uint32_t now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)tv.tv_sec;
}

static void flush_bucket(int idx) {
    if (s_buckets[idx].count == 0) return;
    uint8_t avg = (uint8_t)(s_buckets[idx].sum / s_buckets[idx].count);
    s_last_avg[idx] = avg;
    s_buckets[idx].sum = 0;
    s_buckets[idx].count = 0;
    s_bucket_t0[idx] = now_sec();

    ESP_LOGI(TAG, "窗%d (%lus) 平均=%u", idx, (unsigned long)WIN_SECS[idx], avg);

    /* 注入上层 */
    if (idx + 1 < NOISE_WIN_COUNT) {
        s_buckets[idx + 1].sum += avg;
        s_buckets[idx + 1].count++;
    }
}

void noise_detector_feed(uint8_t level) {
    if (!s_ready) return;
    s_raw_level = level;
    uint32_t now = now_sec();

    /* 检查每个层级是否该收束了 */
    for (int i = 0; i < NOISE_WIN_COUNT; i++) {
        if (s_bucket_t0[i] == 0) s_bucket_t0[i] = now;
        if (now - s_bucket_t0[i] >= WIN_SECS[i]) {
            flush_bucket(i);
        }
    }

    /* 注入 raw → 5s 桶 */
    s_buckets[0].sum += level;
    s_buckets[0].count++;
}

/* ── 噪音采样任务 ── */
static int16_t s_noise_buf[FRAME_SAMPLES];  /* static to avoid stack */

static void noise_task(void *pv) {

    /* 等待 data 分区就绪 */
    vTaskDelay(pdMS_TO_TICKS(5000));
    s_ready = true;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_INTERVAL_MS));

        if (voice_chat_is_recording() || tts_client_is_playing() ||
            session_mgr_is_capturing()) continue;

        double sum_sq = 0.0;
        int total = 0;
        for (int i = 0; i < TOTAL_FRAMES; i++) {
            /* 每次读取前再检查 — 防止TTS在检查后启动导致I2S竞争噪音 */
            if (voice_chat_is_recording() || tts_client_is_playing() ||
                session_mgr_is_capturing()) break;
            int n = es8311_drv_read(s_noise_buf, FRAME_SAMPLES);
            if (n <= 0) break;
            total += n;
            for (int j = 0; j < n; j++) {
                double v = s_noise_buf[j] / 32768.0;
                sum_sq += v * v;
            }
        }

        if (total > 0) {
            double rms = sqrt(sum_sq / total);
            int level = (int)(rms * 100.0);
            if (level > 100) level = 100;
            noise_detector_feed((uint8_t)level);
            ESP_LOGI(TAG, "噪音: %d/100 (rms=%.4f, samples=%d)", level, rms, total);
        } else {
            ESP_LOGW(TAG, "采样失败: total=%d", total);
        }
    }
}

void noise_detector_init(void) {
    memset(s_buckets, 0, sizeof(s_buckets));
    memset(s_bucket_t0, 0, sizeof(s_bucket_t0));
    /* 栈保持内部 RAM — 本任务写 noise.csv (flash 写期间 cache 冻结) */
    xTaskCreate(noise_task, "noise", 8192, NULL, 1, NULL);
    ESP_LOGI(TAG, "噪音检测器就绪 (级联%d层)", NOISE_WIN_COUNT);
}

uint8_t noise_detector_get_level(void) {
    return s_raw_level;
}

uint8_t noise_detector_get_avg(noise_window_t win) {
    if (win >= NOISE_WIN_COUNT) return 0;
    return s_last_avg[win];
}

bool noise_detector_is_loud(void) {
    return s_raw_level > 60;
}

void noise_detector_get_context_str(char *buf, int bufsize) {
    uint8_t s5 = noise_detector_get_avg(NOISE_WIN_5S);
    uint8_t s1h = noise_detector_get_avg(NOISE_WIN_1H);
    uint8_t s24h = noise_detector_get_avg(NOISE_WIN_24H);
    if (s5 || s1h || s24h) {
        snprintf(buf, bufsize,
                 "噪音等级(0-100): 近5秒=%u, 近1小时=%u, 近24小时=%u",
                 s5, s1h, s24h);
    } else {
        buf[0] = '\0';
    }
}

/* ── CSV 写盘 (主循环调用, 单线程安全) ── */
void noise_detector_write_csv(void) {
    /* TTS 播放期间跳过 — flash 写会冻结双核造成音频卡顿, 下个节拍再写 */
    if (tts_client_is_playing()) return;
    static uint8_t s_last_written[NOISE_WIN_COUNT];
    bool any = false;

    for (int i = 0; i < NOISE_WIN_COUNT; i++) {
        if (s_last_avg[i] != s_last_written[i]) {
            s_last_written[i] = s_last_avg[i];
            any = true;
        }
    }
    if (!any) return;

    /* CSV 会跨开机增长 — 超 256KB 重开 (丢弃旧数据) */
    struct stat st;
    if (stat(NOISE_CSV_PATH, &st) == 0 && st.st_size > 256 * 1024)
        remove(NOISE_CSV_PATH);

    FILE *f = fopen(NOISE_CSV_PATH, "a");
    if (!f) return;

    uint32_t now = now_sec();
    for (int i = 0; i < NOISE_WIN_COUNT; i++) {
        if (s_last_avg[i] > 0) {
            fprintf(f, "%lu,%lu,%u\n",
                    (unsigned long)now,
                    (unsigned long)WIN_SECS[i],
                    s_last_avg[i]);
        }
    }
    fclose(f);
}
