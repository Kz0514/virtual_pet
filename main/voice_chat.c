/**
 * Voice chat — press-and-hold recording → ASR → LLM via WebSocket.
 */
#include "voice_chat.h"
#include "board.h"
#include "server_config.h"
#include "api_client.h"
#include "es8311_drv.h"
#include "ws_client.h"
#include "tts_client.h"
#include "notify_overlay.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "voice";

#define RECORD_SECONDS  3
#define SAMPLE_RATE     48000
#define FRAME_MS        20
#define FRAME_SAMPLES   (SAMPLE_RATE * FRAME_MS / 1000)  /* 320 */

static bool s_inited = false;
static volatile bool s_recording = false;

bool voice_chat_is_recording(void) { return s_recording; }

static void ensure_inited(void)
{
    if (s_inited) return;

    es8311_drv_cfg_t cfg = {
        .sample_rate = SAMPLE_RATE, .bits_per_sample = 16, .channels = 1,
        .frame_ms = FRAME_MS, .mic_only = true, .mic_gain_db = 24.0f,
    };
    if (es8311_drv_init(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 init fail");
        return;
    }

    /* Let HPF + VREF settle */
    int16_t dummy[FRAME_SAMPLES];
    for (int i = 0; i < 50; i++) es8311_drv_read(dummy, FRAME_SAMPLES);

    s_inited = true;
    ESP_LOGI(TAG, "Voice chat ready");
}

void voice_chat_init(void) { /* kept for header compat, lazy init on first use */ }

/* ── HTTP response buffer ── */
static char s_resp[1024];
static int  s_resp_len;

static esp_err_t http_cb(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA && s_resp_len + evt->data_len < sizeof(s_resp) - 1) {
        memcpy(s_resp + s_resp_len, evt->data, evt->data_len);
        s_resp_len += evt->data_len; s_resp[s_resp_len] = '\0';
    }
    return ESP_OK;
}

/* ── Simple WAV header ── */
static void wav_hdr(uint8_t *b, uint32_t data_sz) {
    uint32_t fsz = data_sz + 36, br = SAMPLE_RATE * 2;
    memcpy(b, "RIFF", 4); memcpy(b+4, &fsz, 4); memcpy(b+8, "WAVE", 4);
    memcpy(b+12, "fmt ", 4); uint32_t v=16; memcpy(b+16, &v, 4);
    uint16_t w=1; memcpy(b+20, &w, 2); memcpy(b+22, &w, 2);
    v=SAMPLE_RATE; memcpy(b+24, &v, 4); v=br; memcpy(b+28, &v, 4);
    w=2; memcpy(b+32, &w, 2); w=16; memcpy(b+34, &w, 2);
    memcpy(b+36, "data", 4); memcpy(b+40, &data_sz, 4);
}

char *voice_asr_transcribe_pcm(const int16_t *pcm, uint32_t sample_count)
{
    if (!pcm || sample_count == 0) return NULL;

    uint32_t data_bytes = sample_count * 2;
    uint8_t wavh[44];
    wav_hdr(wavh, data_bytes);

    /* Upload */
    char url[384];
    snprintf(url, sizeof(url), "http://%s:%d/api/v1/asr/transcribe?token=%s",
             SERVER_HOST, SERVER_PORT, api_client_get_token());
    esp_http_client_config_t hc = {.url = url, .method = HTTP_METHOD_POST,
                                    .event_handler = http_cb, .timeout_ms = 15000};
    esp_http_client_handle_t cli = esp_http_client_init(&hc);
    if (!cli) { ESP_LOGE(TAG, "HTTP client init fail"); return NULL; }

    const char *bd = "VpetASR";
    char hdr[256]; snprintf(hdr, sizeof(hdr), "multipart/form-data; boundary=%s", bd);
    esp_http_client_set_header(cli, "Content-Type", hdr);

    static const char *p1 = "--VpetASR\r\nContent-Disposition: form-data; name=\"file\"; filename=\"v.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
    static const char *p2 = "\r\n--VpetASR--\r\n";
    int l1 = strlen(p1), l2 = strlen(p2);
    int total = l1 + 44 + (int)data_bytes + l2;

    /* 流式 POST: 零大块拷贝, 直接复用录音缓冲 pcm — 曾因连续两次
     * ~880KB PSRAM 分配静默失败导致 ASR 请求根本没发出 (8MB PSRAM
     * 被字体/动画帧/录音缓冲常驻占满, 大块分配间歇性失败) */
    s_resp_len = 0;
    esp_err_t err = ESP_FAIL;
    if (esp_http_client_open(cli, total) == ESP_OK) {
        int w = 0;
        w += esp_http_client_write(cli, p1, l1);
        w += esp_http_client_write(cli, (char *)wavh, 44);
        w += esp_http_client_write(cli, (char *)pcm, (int)data_bytes);
        w += esp_http_client_write(cli, p2, l2);
        /* fetch_headers 返回 int64_t content-length (≥0=成功, <0=失败),
         * 不是 esp_err_t — 曾把 46/49 字节的响应体长度当错误码拒绝 */
        if (w == total && esp_http_client_fetch_headers(cli) >= 0) {
            char rbuf[256];
            int n;
            while ((n = esp_http_client_read(cli, rbuf, sizeof(rbuf))) > 0) { }
            err = (n < 0) ? ESP_FAIL : ESP_OK;
        }
    }
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);

    if (err != ESP_OK || status != 200) { ESP_LOGE(TAG, "ASR HTTP fail: %d/%d", err, status); return NULL; }

    cJSON *r = cJSON_Parse(s_resp);
    if (!r) { ESP_LOGW(TAG, "ASR 响应解析失败: %.*s", s_resp_len, s_resp); return NULL; }
    cJSON *t = cJSON_GetObjectItem(r, "text");
    char *text = (cJSON_IsString(t) && t->valuestring[0]) ? strdup(t->valuestring) : NULL;
    cJSON_Delete(r);
    ESP_LOGI(TAG, "ASR: %s", text ? text : "(empty)");
    return text;
}

char *voice_chat_record_and_asr(void)
{
    ensure_inited();
    if (!s_inited) return NULL;

    /* 录音前静音扬声器通路并停 TTS — 麦克风与扬声器物理近距,
     * 播放尾音/PA 底噪会被录进 ASR, 边播边录会把上一句内容整段录进去 */
    if (tts_client_is_busy()) {
        ESP_LOGI(TAG, "录音前停止 TTS");
        tts_client_stop();
        for (int i = 0; i < 60 && tts_client_is_busy(); i++)
            vTaskDelay(pdMS_TO_TICKS(10));
    }
    es8311_drv_set_vol(0);   /* 音量门控静音 (PA 常开, DAC 已断电) */

    int total_samples = SAMPLE_RATE * RECORD_SECONDS;
    uint8_t *buf = heap_caps_malloc(44 + total_samples * 2, MALLOC_CAP_SPIRAM);
    if (!buf) return NULL;
    int16_t *pcm = (int16_t *)(buf + 44);

    /* Record */
    ESP_LOGI(TAG, "Recording %ds...", RECORD_SECONDS);
    s_recording = true;
    notify_show(NOTIFY_INFO, "录音中...", 3000);
    int total = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(RECORD_SECONDS * 1000 + 500);
    while (xTaskGetTickCount() < deadline) {
        int rem = total_samples - total;
        if (rem <= 0) break;
        int n = es8311_drv_read(pcm + total, rem < FRAME_SAMPLES ? rem : FRAME_SAMPLES);
        if (n > 0) total += n / sizeof(int16_t);
    }
    s_recording = false;

    char *text = voice_asr_transcribe_pcm(pcm, (uint32_t)total);
    free(buf);
    return text;
}

/* ── FreeRTOS task ── */
void voice_chat_task(void *pvParameter)
{
    char *text = voice_chat_record_and_asr();
    if (text && text[0]) {
        /* Wait for WS to reconnect if needed (up to 5s) */
        for (int retry = 0; retry < 50 && !ws_client_is_connected(); retry++) {
            if (retry == 0) ESP_LOGW(TAG, "WS disconnected, waiting...");
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (ws_client_is_connected()) {
            ws_client_send_chat(text);
        }
        free(text);
    }
    vTaskDelete(NULL);
}
