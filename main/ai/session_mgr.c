/**
 * @file session_mgr.c
 * @brief 连续会话模式 — 半双工多轮语音对话
 *
 * 状态机: IDLE →(进入)→ LISTENING(300ms 底噪校准 + VAD, 10s 窗口)
 *   → RECORDING(录至静音 800ms, 最短 300ms, 最长 15s)
 *   → ASR(只上传人声段)→ WS chat("思考中"通知)→ PLAYING(等 TTS 播完 +1s)
 *   → LISTENING → … ; 聆听窗口无语音 → IDLE("下次再聊~")
 *
 * 关键设计:
 * - VAD 能量法: 20ms 块 RMS, 起点 = 2.5×底噪连续 3 块, 止点 = 1.5×底噪
 *   连续 800ms (迟滞防抖动)
 * - 2s 回看环形缓冲: 检出起点后往前补 300ms — 防吞字头
 * - 只把人声段送 ASR — 静音不上传 (按秒计费不烧钱)
 * - 半双工: 聆听/录音前停 TTS 关 PA; 播放期间麦克风关闭 (扬声器近距耦合)
 */
#include "session_mgr.h"
#include "es8311_drv.h"
#include "tts_client.h"
#include "voice_chat.h"
#include "ws_client.h"
#include "notify_overlay.h"
#include "chat_bubble.h"
#include "noise_detector.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>

static const char *TAG = "sess";

#define SAMPLE_RATE    48000
#define BLOCK_MS       20
#define BLOCK_SAMPLES  (SAMPLE_RATE * BLOCK_MS / 1000)     /* 960 */

#define CALIBRATE_MS     300      /* 底噪校准时长 */
#define LISTEN_WINDOW_MS 10000    /* 每次聆听窗口 */
#define MAX_RECORD_MS    15000    /* 单段录音上限 */
#define MIN_SPEECH_MS    300      /* 最短人声 (更短丢弃) */
#define SILENCE_END_MS   800      /* 静音判定止点 */
#define PRE_SPEECH_MS    300      /* 起点往前补的字头 */

#define LOOKBACK_SAMPLES (SAMPLE_RATE * 2)   /* 2s 回看 */
#define REC_MAX_SAMPLES  (SAMPLE_RATE * MAX_RECORD_MS / 1000)

typedef enum {
    SESS_IDLE,
    SESS_LISTENING,     /* 含底噪校准 */
    SESS_RECORDING,
    SESS_ASR,
    SESS_WAIT_REPLY,
    SESS_PLAYING,
} sess_state_t;

static volatile sess_state_t s_state = SESS_IDLE;
static volatile bool s_enter_req = false;
static TaskHandle_t s_task = NULL;

static int16_t *s_lookback = NULL;   /* 2s 回看环形 (PSRAM, 192KB) */
static uint32_t  s_lb_wr = 0;
static int16_t *s_rec = NULL;        /* 录音缓冲 (PSRAM, 1.44MB) */
static uint32_t  s_rec_len = 0;
static float     s_start_thr = 0.0125f;   /* VAD 起点阈值 (截断上限) */
static float     s_stop_thr  = 0.0075f;   /* VAD 止点阈值 */
static int16_t   s_block[BLOCK_SAMPLES];

static void sess_set_state(sess_state_t st)
{
    ESP_LOGI(TAG, "state %d → %d", (int)s_state, (int)st);
    s_state = st;
}

static void stop_tts_and_silence(void)
{
    if (tts_client_is_busy()) {
        tts_client_stop();
        for (int i = 0; i < 60 && tts_client_is_busy(); i++)
            vTaskDelay(pdMS_TO_TICKS(10));
    }
    es8311_drv_set_vol(0);
}

static void lb_push(const int16_t *data, uint32_t samples)
{
    for (uint32_t i = 0; i < samples; i++) {
        s_lookback[s_lb_wr] = data[i];
        s_lb_wr = (s_lb_wr + 1) % LOOKBACK_SAMPLES;
    }
}

/* 取回看缓冲最近 n 个样本 (n <= LOOKBACK_SAMPLES) */
static void lb_copy_last(uint32_t n, int16_t *dst)
{
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (s_lb_wr + LOOKBACK_SAMPLES - n + i) % LOOKBACK_SAMPLES;
        dst[i] = s_lookback[idx];
    }
}

static float block_rms(const int16_t *data, uint32_t samples)
{
    double sum = 0.0;
    for (uint32_t i = 0; i < samples; i++) {
        double v = data[i] / 32768.0;
        sum += v * v;
    }
    return (float)sqrt(sum / samples);
}

/* ── 滑动窗口底噪跟踪 ──
 * 每块 RMS 进 2s 窗口, 窗口最小值 = 当前噪声底噪 (语音只抬升 RMS,
 * 不影响最小值); 聆听期间麦克风本来就在采样, 顺手自适应 —
 * 环境变安静后阈值自动下调, 环境变吵后 2s 内跟上.
 * 初始值用环境噪音检测器读数播种 (不受敲击等瞬态污染). */
#define FLOOR_WIN_BLOCKS 100   /* 2s @20ms */

static float s_rms_win[FLOOR_WIN_BLOCKS];
static int   s_rms_idx = 0;
static bool  s_floor_seeded = false;   /* 窗口只在会话开始播种一次, 跨轮保持 */

static void floor_window_reset(float seed)
{
    for (int i = 0; i < FLOOR_WIN_BLOCKS; i++) s_rms_win[i] = seed;
    s_rms_idx = 0;
}

static void floor_window_push(float rms)
{
    s_rms_win[s_rms_idx] = rms;
    s_rms_idx = (s_rms_idx + 1) % FLOOR_WIN_BLOCKS;
}

static float floor_window_min(void)
{
    float m = s_rms_win[0];
    for (int i = 1; i < FLOOR_WIN_BLOCKS; i++)
        if (s_rms_win[i] < m) m = s_rms_win[i];
    return m;
}

/* 由底噪重算 VAD 阈值 (上下限截断 — 上限防满量程失效, 下限防绝对静音过敏) */
static void update_thresholds(float floor)
{
    s_start_thr = floor * 2.2f;
    if (s_start_thr < 0.05f) s_start_thr = 0.05f;
    if (s_start_thr > 0.45f) s_start_thr = 0.45f;
    s_stop_thr = floor * 1.4f;
    if (s_stop_thr < 0.03f) s_stop_thr = 0.03f;
    if (s_stop_thr > 0.25f) s_stop_thr = 0.25f;
}

/* 等待回复播放完成: WS 回调里 chat_seq++ 先于 tts_speak, 醒来时 TTS
 * 可能尚未启动 — 先等 1s 启动宽限; 之后等 busy 结束 + 1s 停顿 */
static void wait_playback(void)
{
    for (int i = 0; i < 10 && !tts_client_is_busy(); i++)
        vTaskDelay(pdMS_TO_TICKS(100));
    while (tts_client_is_busy())
        vTaskDelay(pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(1000));   /* 播完 +1s 再回聆听 */
}

/* 300ms 麦克风预填 — 回看缓冲装满当前音频 (防起点补字头时夹带旧数据),
 * 同时把真实环境 RMS 推入滑动窗口 — 每轮聆听前阈值已贴合当前环境,
 * 不再需要重新适应 (曾因每轮重置窗口导致第二轮起 VAD 前 2s 失灵) */
static void prefill_lookback(void)
{
    s_lb_wr = 0;
    for (int i = 0; i < CALIBRATE_MS / BLOCK_MS; i++) {
        int n = es8311_drv_read(s_block, BLOCK_SAMPLES);
        if (n <= 0) { vTaskDelay(pdMS_TO_TICKS(BLOCK_MS)); continue; }
        int got = n / sizeof(int16_t);
        lb_push(s_block, got);
        floor_window_push(block_rms(s_block, got));
    }
    update_thresholds(floor_window_min());
    ESP_LOGI(TAG, "底噪(实时)=%.4f → 阈值 %.4f/%.4f",
             floor_window_min(), s_start_thr, s_stop_thr);
}

static void sess_task(void *pv)
{
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   /* 等待进入 */
        if (!s_enter_req) continue;
        s_enter_req = false;

        ESP_LOGI(TAG, "会话开始");
        stop_tts_and_silence();

        /* 底噪窗口播种 (仅会话开始一次, 之后跨轮自适应保持) */
        if (!s_floor_seeded) {
            float seed = (float)noise_detector_get_level() / 100.0f;
            if (seed < 0.005f) seed = 0.005f;
            if (seed > 0.35f)  seed = 0.35f;
            floor_window_reset(seed);
            s_floor_seeded = true;
        }

        uint32_t asr_fails = 0;

        while (1) {
            /* ════ 聆听窗口 (含底噪校准 + VAD) ════ */
            /* 每轮聆听前音量归零 (PA 常开) */
            es8311_drv_set_vol(0);
            sess_set_state(SESS_LISTENING);
            prefill_lookback();
            notify_show(NOTIFY_INFO, "在听~", LISTEN_WINDOW_MS + 1000);

            uint32_t win_blocks = LISTEN_WINDOW_MS / BLOCK_MS;
            uint32_t loud_run = 0, silent_run = 0;
            bool recording = false, got_speech = false, late_reply = false;
            s_rec_len = 0;

            for (uint32_t t = 0; t < win_blocks; t++) {
                /* 回复晚到 (WS 回调延迟启动 TTS) — 中止聆听防抢录 */
                if (tts_client_is_busy()) { late_reply = true; break; }
                int n = es8311_drv_read(s_block, BLOCK_SAMPLES);
                if (n <= 0) { vTaskDelay(pdMS_TO_TICKS(BLOCK_MS)); continue; }
                int got = n / sizeof(int16_t);
                float rms = block_rms(s_block, got);

                if (!recording) {
                    lb_push(s_block, got);
                    /* 聆听期间自适应底噪 — 阈值跟着环境实时走 */
                    floor_window_push(rms);
                    update_thresholds(floor_window_min());
                    if (rms > s_start_thr) {
                        if (++loud_run >= 2) {
                            /* 检出人声 — 回看补 300ms 字头, 开始录音 */
                            recording = true;
                            notify_hide();
                            uint32_t pre = SAMPLE_RATE * PRE_SPEECH_MS / 1000;
                            lb_copy_last(pre, s_rec);
                            s_rec_len = pre;
                            memcpy(s_rec + s_rec_len, s_block, got * 2);
                            s_rec_len += got;
                            silent_run = 0;
                            ESP_LOGI(TAG, "检出人声 (rms=%.4f)", rms);
                        }
                    } else {
                        loud_run = 0;
                    }
                } else {
                    memcpy(s_rec + s_rec_len, s_block, got * 2);
                    s_rec_len += got;
                    if (rms < s_stop_thr) silent_run++;
                    else silent_run = 0;

                    if (silent_run * BLOCK_MS >= SILENCE_END_MS ||
                        s_rec_len >= REC_MAX_SAMPLES) {
                        got_speech = true;
                        break;
                    }
                }
            }
            /* 窗口结束仍在录音 = 说到窗口尾, 也算一段话 */
            if (recording && !got_speech &&
                s_rec_len >= SAMPLE_RATE * MIN_SPEECH_MS / 1000)
                got_speech = true;

            if (late_reply) {
                ESP_LOGI(TAG, "回复晚到 — 回播放等待");
                wait_playback();
                continue;
            }

            if (!got_speech) {
                ESP_LOGI(TAG, "聆听超时无语音 — 会话结束");
                break;
            }
            if (s_rec_len < SAMPLE_RATE * MIN_SPEECH_MS / 1000) {
                ESP_LOGI(TAG, "人声太短 (%dms) — 丢弃继续聆听",
                         (int)(s_rec_len * 1000 / SAMPLE_RATE));
                continue;
            }
            sess_set_state(SESS_RECORDING);
            ESP_LOGI(TAG, "录音结束: %dms", (int)(s_rec_len * 1000 / SAMPLE_RATE));

            /* ════ ASR (只传人声段) ════ */
            sess_set_state(SESS_ASR);
            notify_show(NOTIFY_INFO, "思考中…", 15000);
            char *text = voice_asr_transcribe_pcm(s_rec, s_rec_len);
            if (!text || !text[0]) {
                if (text) free(text);
                if (++asr_fails >= 3) {
                    chat_bubble_show("网络不可用，下次再聊~", 4000);
                    break;
                }
                ESP_LOGW(TAG, "ASR 失败 (%d/3) — 回聆听 (PSRAM 空闲 %u KB)",
                         (int)asr_fails,
                         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
                continue;
            }
            asr_fails = 0;
            ESP_LOGI(TAG, "ASR: %s", text);

            /* ════ 发送 + 等回复 ════ */
            if (!ws_client_is_connected()) {
                free(text);
                chat_bubble_show("网络不可用…", 4000);
                break;
            }
            uint32_t seq0 = ws_client_get_chat_seq();
            ws_client_send_chat(text);
            free(text);

            sess_set_state(SESS_WAIT_REPLY);
            for (int i = 0; i < 450; i++) {   /* 45s, 100ms 步进 */
                vTaskDelay(pdMS_TO_TICKS(100));
                if (ws_client_get_chat_seq() != seq0) break;
                if (!ws_client_is_connected()) break;
            }
            if (!ws_client_is_connected()) {
                chat_bubble_show("网络不可用…", 4000);
                break;
            }
            if (ws_client_get_chat_seq() == seq0) {
                ESP_LOGW(TAG, "等回复超时 — 回聆听");
                continue;
            }

            notify_hide();

            /* ════ PLAYING: 等 TTS 播完 ════ */
            sess_set_state(SESS_PLAYING);
            wait_playback();
        }

        /* ════ 会话结束 ════ */
        sess_set_state(SESS_IDLE);
        chat_bubble_show("下次再聊~", 4000);
        es8311_drv_set_vol(0);
        ESP_LOGI(TAG, "会话结束");
    }
}

void session_mgr_init(void)
{
    s_lookback = heap_caps_malloc(LOOKBACK_SAMPLES * 2, MALLOC_CAP_SPIRAM);
    s_rec = heap_caps_malloc(REC_MAX_SAMPLES * 2, MALLOC_CAP_SPIRAM);
    if (!s_lookback || !s_rec) {
        ESP_LOGE(TAG, "PSRAM 缓冲分配失败 — 会话模式不可用");
        return;
    }
    xTaskCreateWithCaps(sess_task, "sess", 16384, NULL, 5, &s_task,
                        MALLOC_CAP_SPIRAM);   /* 12KB 曾在 esp_http_client+cJSON
                                               调用链上栈溢出双异常 */
    ESP_LOGI(TAG, "会话模式就绪 (回看 %dms, 录音上限 %ds)",
             LOOKBACK_SAMPLES * 1000 / SAMPLE_RATE, MAX_RECORD_MS / 1000);
}

void session_mgr_enter(void)
{
    if (s_state == SESS_IDLE) {
        s_enter_req = true;
        if (s_task) xTaskNotifyGive(s_task);
    } else {
        ESP_LOGI(TAG, "会话进行中, 忽略进入请求 (打断机制已移除)");
    }
}

bool session_mgr_is_active(void)
{
    return s_state != SESS_IDLE;
}

bool session_mgr_is_playing(void)
{
    return s_state == SESS_PLAYING || s_state == SESS_WAIT_REPLY;
}

bool session_mgr_is_capturing(void)
{
    return s_state == SESS_LISTENING || s_state == SESS_RECORDING ||
           s_state == SESS_ASR;
}

void session_mgr_log_stack(void)
{
    if (s_task)
        ESP_LOGI(TAG, "sess 任务栈水位 %u B", (unsigned)uxTaskGetStackHighWaterMark(s_task));
}
