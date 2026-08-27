/**
 * ASR client — 上传 PCM 到服务器识别为文本 (HTTP multipart)。
 * 原 voice_chat 录音通路 (press-and-hold → record_and_asr → WS 发送)
 * 已被会话模式 VAD 录音取代 (session_mgr), 旧通路 ④-1 随搬迁删除。
 */
#include "asr_client.h"
#include "board.h"
#include "server_config.h"
#include "api_client.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "asr";

#define SAMPLE_RATE 48000

static volatile bool s_recording = false;

bool asr_is_recording(void) { return s_recording; }

/* ── HTTP response buffer ── */
static char s_resp[1024];
static int s_resp_len;

static esp_err_t http_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && s_resp_len + evt->data_len < sizeof(s_resp) - 1) {
        memcpy(s_resp + s_resp_len, evt->data, evt->data_len);
        s_resp_len += evt->data_len;
        s_resp[s_resp_len] = '\0';
    }
    return ESP_OK;
}

/* ── Simple WAV header ── */
static void wav_hdr(uint8_t *b, uint32_t data_sz)
{
    uint32_t fsz = data_sz + 36, br = SAMPLE_RATE * 2;
    memcpy(b, "RIFF", 4);
    memcpy(b + 4, &fsz, 4);
    memcpy(b + 8, "WAVE", 4);
    memcpy(b + 12, "fmt ", 4);
    uint32_t v = 16;
    memcpy(b + 16, &v, 4);
    uint16_t w = 1;
    memcpy(b + 20, &w, 2);
    memcpy(b + 22, &w, 2);
    v = SAMPLE_RATE;
    memcpy(b + 24, &v, 4);
    v = br;
    memcpy(b + 28, &v, 4);
    w = 2;
    memcpy(b + 32, &w, 2);
    w = 16;
    memcpy(b + 34, &w, 2);
    memcpy(b + 36, "data", 4);
    memcpy(b + 40, &data_sz, 4);
}

char *asr_transcribe_pcm(const int16_t *pcm, uint32_t sample_count)
{
    if (!pcm || sample_count == 0) return NULL;

    uint32_t data_bytes = sample_count * 2;
    uint8_t wavh[44];
    wav_hdr(wavh, data_bytes);

    /* Upload */
    char url[384];
    snprintf(url, sizeof(url), "http://%s:%d/api/v1/asr/transcribe?token=%s",
             SERVER_HOST, SERVER_PORT, api_client_get_token());
    esp_http_client_config_t hc = {.url = url, .method = HTTP_METHOD_POST, .event_handler = http_cb, .timeout_ms = 15000};
    esp_http_client_handle_t cli = esp_http_client_init(&hc);
    if (!cli) {
        ESP_LOGE(TAG, "HTTP client init fail");
        return NULL;
    }

    const char *bd = "VpetASR";
    char hdr[256];
    snprintf(hdr, sizeof(hdr), "multipart/form-data; boundary=%s", bd);
    esp_http_client_set_header(cli, "Content-Type", hdr);

    static const char *p1 = "--VpetASR\r\nContent-Disposition: form-data; name=\"file\"; filename=\"v.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
    static const char *p2 = "\r\n--VpetASR--\r\n";
    int l1 = strlen(p1), l2 = strlen(p2);
    int total = l1 + 44 + (int)data_bytes + l2;

    /* 流式 POST: 零大块拷贝, 直接复用录音缓冲 pcm — 避免再次申请大块
     * PSRAM (8MB PSRAM 被字体/动画帧/录音缓冲常驻占满, 大块分配易失败) */
    s_resp_len = 0;
    esp_err_t err = ESP_FAIL;
    if (esp_http_client_open(cli, total) == ESP_OK) {
        int w = 0;
        w += esp_http_client_write(cli, p1, l1);
        w += esp_http_client_write(cli, (char *)wavh, 44);
        w += esp_http_client_write(cli, (char *)pcm, (int)data_bytes);
        w += esp_http_client_write(cli, p2, l2);
        /* fetch_headers 返回 int64_t content-length (≥0=成功, <0=失败),
         * 不是 esp_err_t, 不能按 esp_err_t 判负 */
        if (w == total && esp_http_client_fetch_headers(cli) >= 0) {
            char rbuf[256];
            int n;
            while ((n = esp_http_client_read(cli, rbuf, sizeof(rbuf))) > 0) {
            }
            err = (n < 0) ? ESP_FAIL : ESP_OK;
        }
    }
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "ASR HTTP fail: %d/%d", err, status);
        return NULL;
    }

    cJSON *r = cJSON_Parse(s_resp);
    if (!r) {
        ESP_LOGW(TAG, "ASR 响应解析失败: %.*s", s_resp_len, s_resp);
        return NULL;
    }
    cJSON *t = cJSON_GetObjectItem(r, "text");
    char *text = (cJSON_IsString(t) && t->valuestring[0]) ? strdup(t->valuestring) : NULL;
    cJSON_Delete(r);
    ESP_LOGI(TAG, "ASR: %s", text ? text : "(empty)");
    return text;
}