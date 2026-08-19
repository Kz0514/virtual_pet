/**
 * @file tts_client.c
 * @brief TTS 流式 — chunked HTTP → ring buffer → 边下边播
 *
 * Architecture:
 *   download_task (prio 6): recv → chunked_decode → ringbuf_put
 *   playback_task (prio 5): ringbuf_get → es8311_drv_write (DMA pacing)
 *
 * Ring buffer: 256KB SPSC, lock-free — wr only by producer, rd only by consumer.
 * Unsigned 32-bit subtraction wr-rd always correct for byte count (max gap=256KB << 2^32).
 */
#include "tts_client.h"
#include "server_config.h"
#include "api_client.h"
#include "es8311_drv.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "tts";

/* ── Ring buffer (SPSC lock-free) ── */
#define RB_SIZE (1024 * 1024)  /* 1 MB, power of 2 for fast modulo */

static uint8_t   *rb_buf;       /* SPIRAM */
static volatile uint32_t rb_wr; /* producer index (bytes), monotonic, overflow OK */
static volatile uint32_t rb_rd; /* consumer index (bytes) */
static volatile bool    rb_done;   /* download complete flag */
static TaskHandle_t     s_playback_task;  /* for task notification */
bool tts_client_is_playing(void) { return s_playback_task != NULL; }

/* 全周期忙碌标志 — 防止连续TTS请求重置环形缓冲导致噪音/卡死 */
static volatile bool s_busy = false;
static volatile bool s_downloading = false;
static volatile uint32_t s_playback_start_tick = 0;

/* ── 协作式中断 (不杀任务) ── */
static volatile bool     s_stop_req = false;   /* 停止请求 — 两任务轮询 */
static volatile uint32_t s_gen = 0;            /* 世代号 — 每次新会话递增 */
static volatile uint32_t s_dl_gen = 0;         /* 当前 download 任务的世代 (rb_put 守卫) */
static SemaphoreHandle_t s_api_mutex = NULL;   /* 串行化 stop/speak/interrupt 入口 */

bool tts_client_is_downloading(void) { return s_downloading; }
bool tts_client_is_busy(void) { return s_busy; }

/* 播放已开始的毫秒数 (0=未开始) — 供文字同步使用 */
uint32_t tts_client_get_playback_ms(void) {
    if (!s_playback_task || s_playback_start_tick == 0) return 0;
    return (xTaskGetTickCount() - s_playback_start_tick) * portTICK_PERIOD_MS;
}

static inline uint32_t rb_avail(void) { return rb_wr - rb_rd; }
static inline uint32_t rb_free(void)  { return RB_SIZE - (rb_wr - rb_rd); }

/* Called by download task; blocks until all bytes written.
 * Notifies playback task after adding data.
 * 世代守卫: 被打断的旧 download 任务不得再向新会话的缓冲写数据. */
static void rb_put(const uint8_t *data, uint32_t len) {
    if (s_dl_gen != s_gen) return;
    while (len > 0) {
        uint32_t free = rb_free();
        if (free == 0) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        uint32_t n = (len < free) ? len : free;
        uint32_t pos = rb_wr & (RB_SIZE - 1);
        uint32_t first = RB_SIZE - pos;
        if (n <= first) {
            memcpy(rb_buf + pos, data, n);
        } else {
            memcpy(rb_buf + pos, data, first);
            memcpy(rb_buf, data + first, n - first);
        }
        rb_wr += n;
        data += n;
        len -= n;
    }
    /* Wake playback task — data arrived */
    if (s_playback_task) xTaskNotifyGive(s_playback_task);
}

/* Non-blocking: read up to max_len, return bytes read (may be 0) */
static uint32_t rb_try_get(uint8_t *data, uint32_t max_len) {
    uint32_t avail = rb_avail();
    if (avail == 0) return 0;
    uint32_t n = (max_len < avail) ? max_len : avail;
    uint32_t pos = rb_rd & (RB_SIZE - 1);
    uint32_t first = RB_SIZE - pos;
    if (n <= first) {
        memcpy(data, rb_buf + pos, n);
    } else {
        memcpy(data, rb_buf + pos, first);
        memcpy(data + first, rb_buf, n - first);
    }
    rb_rd += n;
    return n;
}

/* ── Download task ── */
typedef struct { char *text; char *instruction; } tts_args_t;

/* ── chunked transfer 解码状态机 ── */
typedef enum { CH_SIZE, CH_SIZE_LF, CH_DATA, CH_DATA_CR, CH_DATA_LF, CH_DONE } ch_state_t;

typedef struct {
    ch_state_t st;
    uint32_t   remaining;  /* CH_DATA 剩余字节 */
    uint32_t   size_val;   /* 当前 chunk 大小 (十六进制) */
} chunk_parser_t;

static void cp_reset(chunk_parser_t *p)
{
    p->st = CH_SIZE; p->remaining = 0; p->size_val = 0;
}

/* 喂入原始字节, 只把 chunk 数据写入 ring buffer (宽容解析, 容忍 \r 丢失) */
static void cp_feed(chunk_parser_t *p, const uint8_t *d, uint32_t len)
{
    uint32_t i = 0;
    while (i < len && p->st != CH_DONE) {
        uint8_t c = d[i];
        switch (p->st) {
        case CH_SIZE:  /* 十六进制大小行 */
            if (c >= '0' && c <= '9')      p->size_val = p->size_val * 16 + (c - '0');
            else if (c >= 'a' && c <= 'f') p->size_val = p->size_val * 16 + (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') p->size_val = p->size_val * 16 + (c - 'A' + 10);
            else if (c == '\r')            p->st = CH_SIZE_LF;
            i++;
            break;
        case CH_SIZE_LF:
            i++;
            if (p->size_val == 0) { p->st = CH_DONE; break; }
            p->st = CH_DATA; p->remaining = p->size_val;
            break;
        case CH_DATA: {
            uint32_t avail = len - i;
            uint32_t n = (avail < p->remaining) ? avail : p->remaining;
            if (n > 0) { rb_put(d + i, n); i += n; p->remaining -= n; }
            if (p->remaining == 0) p->st = CH_DATA_CR;
            break;
        }
        case CH_DATA_CR:
            i++;
            p->st = (c == '\r') ? CH_DATA_LF : CH_SIZE;
            if (p->st == CH_SIZE) p->size_val = 0;
            break;
        case CH_DATA_LF:
            i++;
            p->st = CH_SIZE; p->size_val = 0;
            break;
        case CH_DONE:
            return;
        }
    }
}

/* download 任务统一退出 (永不返回) — 世代守卫: 旧任务不再触碰共享状态 */
static void dl_exit(tts_args_t *args, int sock)
{
    if (sock >= 0) close(sock);
    if (s_dl_gen == s_gen) {
        rb_done = true;          /* EOF/停止 — 通知 playback 排空 */
        s_downloading = false;
    }
    if (args) {
        free(args->text);
        if (args->instruction) free(args->instruction);
        free(args);
    }
    vTaskDelete(NULL);
}

static void download_task(void *pv) {
    tts_args_t *args = (tts_args_t *)pv;
    s_downloading = true;
    s_dl_gen = s_gen;
    if (!args || !args->text) { dl_exit(args, -1); return; }
    const char *text = args->text;
    const char *instruction = args->instruction;
    ESP_LOGI(TAG, "TTS task start: '%s'", text);
    const char *token = api_client_get_token();
    if (!token || !token[0]) {
        ESP_LOGE(TAG, "TTS: no token!");
        dl_exit(args, -1);
        return;
    }

    /* URL-encode text (Chinese char = 3 bytes → 9 URL chars, need headroom)
     * 大缓冲全部 static — TTS 单飞, 省栈防碎片化 (栈申请失败会整段丢 TTS) */
    static char enc[1536];
    const char *s = text;
    char *d = enc, *e = enc + sizeof(enc) - 1;
    while (*s && d < e) {
        if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') ||
            (*s >= '0' && *s <= '9') || *s == '-' || *s == '_' ||
            *s == '.' || *s == '~')
            *d++ = *s++;
        else
            d += snprintf(d, e - d, "%%%02X", (uint8_t)*s++);
    }
    *d = 0;
    ESP_LOGI(TAG, "TTS: %s", text);
    if (instruction && instruction[0])
        ESP_LOGI(TAG, "  inst: %s", instruction);

    /* HTTP/1.0 POST */
    static char req[3584];
    if (instruction && instruction[0]) {
        static char enc_inst[512];
        const char *si = instruction;
        char *di = enc_inst, *ei = enc_inst + sizeof(enc_inst) - 1;
        while (*si && di < ei) {
            if ((*si >= 'A' && *si <= 'Z') || (*si >= 'a' && *si <= 'z') ||
                (*si >= '0' && *si <= '9') || *si == '-' || *si == '_' ||
                *si == '.' || *si == '~' || *si == ' ')
                *di++ = (*si == ' ') ? '+' : *si++;
            else
                di += snprintf(di, ei - di, "%%%02X", (uint8_t)*si++);
        }
        *di = 0;
        snprintf(req, sizeof(req),
            "POST /api/v1/tts/synthesize-stream?text=%s"
            "&instruction=%s&token=%s"
            " HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
            enc, enc_inst, token, SERVER_HOST);
    } else {
        snprintf(req, sizeof(req),
            "POST /api/v1/tts/synthesize-stream?text=%s"
            "&token=%s"
            " HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
            enc, token, SERVER_HOST);
    }

    struct addrinfo h = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *r;
    char ps[8];
    snprintf(ps, sizeof(ps), "%d", SERVER_PORT);
    if (getaddrinfo(SERVER_HOST, ps, &h, &r) || !r) {
        ESP_LOGE(TAG, "TTS: DNS fail");
        dl_exit(args, -1);
        return;
    }
    int sock = socket(r->ai_family, r->ai_socktype, 0);
    if (sock < 0 || connect(sock, r->ai_addr, r->ai_addrlen) < 0) {
        ESP_LOGE(TAG, "TTS: connect fail");
        freeaddrinfo(r);
        dl_exit(args, sock);
        return;
    }
    freeaddrinfo(r);

    /* 200ms 收超时 — 停止标志能被及时轮询 */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    send(sock, req, strlen(req), 0);

    /* Read & strip HTTP header */
    static char t[4096];
    int n = recv(sock, t, sizeof(t) - 1, 0);
    if (n <= 0) { dl_exit(args, sock); return; }
    t[n] = 0;
    char *bd = strstr(t, "\r\n\r\n");
    if (!bd) bd = strstr(t, "\n\n");
    if (!bd) { dl_exit(args, sock); return; }
    if (!strstr(t, " 200 ")) {
        ESP_LOGE(TAG, "TTS HTTP 非200: %.12s", t);
        dl_exit(args, sock);
        return;
    }
    bd += (bd[0] == '\r') ? 4 : 2;  /* skip past header boundary */
    int leftover = n - (bd - t);

    TickType_t t_start = xTaskGetTickCount();
    uint32_t dl_bytes = leftover > 0 ? leftover : 0;  /* 速率诊断 */
    uint32_t dl_last = 0;
    TickType_t dl_last_t = t_start;

    /* chunked 解码 — 边收边解边入环, 流式开播 */
    chunk_parser_t cp;
    cp_reset(&cp);
    if (leftover > 0) {
        cp_feed(&cp, (uint8_t *)bd, leftover);
        if (s_playback_task) xTaskNotifyGive(s_playback_task);
    }

    static uint8_t buf[4096];
    while (cp.st != CH_DONE) {
        n = recv(sock, buf, sizeof(buf), 0);
        if (n < 0) {
            /* SO_RCVTIMEO 超时 — 轮询停止标志后继续等 */
            if (s_stop_req) break;
            continue;
        }
        if (n == 0) break;   /* 连接关闭 */
        dl_bytes += n;
        cp_feed(&cp, buf, n);
        /* 每 5s 打印下载速率 — 卡顿时可区分: 服务端合成慢 vs 设备网络慢 */
        if (xTaskGetTickCount() - dl_last_t >= pdMS_TO_TICKS(5000)) {
            uint32_t dt_ms = (xTaskGetTickCount() - dl_last_t) * portTICK_PERIOD_MS;
            ESP_LOGI(TAG, "DL rate: %d KB/s (总 %dKB)",
                     (int)((dl_bytes - dl_last) * 1000 / (dt_ms * 1024)),
                     (int)(dl_bytes / 1024));
            dl_last = dl_bytes;
            dl_last_t = xTaskGetTickCount();
        }
        if (s_playback_task) xTaskNotifyGive(s_playback_task);
    }

    ESP_LOGI(TAG, "Download done (%dms)%s",
             (int)((xTaskGetTickCount() - t_start) * portTICK_PERIOD_MS),
             s_stop_req ? " [stopped]" : "");
    dl_exit(args, sock);
}

/* ── Playback task ── */
#define PLAY_CHUNK_SAMPLES 3840  /* 80ms @48kHz — big enough to avoid DMA underrun */

static void playback_task(void *pv) {
    s_playback_task = xTaskGetCurrentTaskHandle();
    uint32_t my_gen = s_gen;

    /* 预冲 1.2s 再开播 — flash 写已全部门控; 1.2s 吸收动画加载与
     * DashScope 短抖动 (长停顿无法靠缓冲, 属外部服务波动) */
    #define MIN_START_SAMPLES 57600  /* 1.2s @48kHz */
    while (rb_avail() < MIN_START_SAMPLES * 2 && !rb_done) {
        if (s_stop_req) break;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }
    if ((rb_avail() == 0 && rb_done) || (s_stop_req && rb_avail() == 0)) {
        /* 空音频/失败/未开播即停止 — 只清自己世代的会话 */
        if (my_gen == s_gen) {
            s_playback_task = NULL;
            s_busy = false;   /* 必须清 — 否则TTS永久拒绝新请求 */
            s_playback_start_tick = 0;
        }
        vTaskDelete(NULL);
        return;
    }

    es8311_drv_set_vol(100);
    esp_codec_dev_handle_t dac = es8311_get_dac_handle();
    if (dac) esp_codec_dev_write_reg(dac, 0x32, 0xCC);

    /* Pre-fill I2S DMA with silence to clear residual */
    int16_t pre_sil[960] = {0};
    es8311_drv_write(pre_sil, 480);
    es8311_drv_write(pre_sil, 480);
    vTaskDelay(pdMS_TO_TICKS(20));

    int16_t *chunk = heap_caps_malloc(PLAY_CHUNK_SAMPLES * 2, MALLOC_CAP_SPIRAM);
    if (!chunk) {
        if (my_gen == s_gen) {
            s_playback_task = NULL;
            s_busy = false;
            s_playback_start_tick = 0;
        }
        vTaskDelete(NULL);
        return;
    }

    int total_played = 0;
    int underruns = 0;     /* 环形缓冲空等待次数 — 诊断卡顿来源 */
    bool stopped = false;
    TickType_t t_start = xTaskGetTickCount();
    s_playback_start_tick = t_start;   /* 供文字同步 */

    while (1) {
        if (s_stop_req) { stopped = true; break; }
        /* Drain ring buffer: always read in even bytes (PCM sample = 2 bytes) */
        uint32_t avail = rb_avail();
        if (avail >= 4) {
            uint32_t to_read = avail;
            if (to_read > PLAY_CHUNK_SAMPLES * 2) to_read = PLAY_CHUNK_SAMPLES * 2;
            to_read &= ~1u;  /* force even — never break a sample */
            uint32_t got = rb_try_get((uint8_t *)chunk, to_read);
            if (got >= 4) {
                int samples = got / 2;
                es8311_drv_write(chunk, samples);
                total_played += samples;
                /* es8311_drv_write blocks until I2S DMA has room —
                 * natural pacing. No vTaskDelay needed. */
            }
        } else if (rb_done && avail == 0) {
            break;
        } else {
            /* Buffer low — wait for download to deliver more data */
            if (!rb_done) underruns++;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
        }
    }

    /* Flush I2S DMA pipeline with silence — DMA holds ~1.2s.
     * 正常结束冲 1200ms 排空残留; 被打断只冲 200ms, 尽快让位新段 */
    int16_t sil[960] = {0};
    int flush_iters = stopped ? 10 : 60;   /* 200ms vs 1200ms */
    for (int i = 0; i < flush_iters; i++)
        es8311_drv_write(sil, 960);
    vTaskDelay(pdMS_TO_TICKS(100));

    heap_caps_free(chunk);

    es8311_drv_set_vol(0);

    int duration_ms = (total_played * 1000) / 48000;
    TickType_t elapsed = xTaskGetTickCount() - t_start;
    ESP_LOGI(TAG, "PCM: %d samples (%dms @48kHz) played in %dms underruns=%d%s",
             total_played, duration_ms,
             (int)(elapsed * portTICK_PERIOD_MS), underruns,
             stopped ? " [stopped]" : "");
    /* 世代守卫: 新会话已接管时不碰共享状态 */
    if (my_gen == s_gen) {
        s_playback_task = NULL;
        s_busy = false;
        s_playback_start_tick = 0;
    }
    vTaskDelete(NULL);
}

/* ── Public API ── */

void tts_client_init(void) {
    if (!s_api_mutex) s_api_mutex = xSemaphoreCreateMutex();
    if (rb_buf) return;  /* already allocated */
    rb_buf = heap_caps_malloc(RB_SIZE, MALLOC_CAP_SPIRAM);
    if (rb_buf) {
        ESP_LOGI(TAG, "TTS streaming ready (ringbuf %dKB)", RB_SIZE / 1024);
    } else {
        ESP_LOGE(TAG, "ringbuf alloc FAILED! SPIRAM available?");
    }
}

static bool tts_speak_internal(const char *text, const char *instruction) {
    if (!text || !text[0]) return false;
    /* 忙碌时拒绝新请求 — 防止环形缓冲被并发重置 */
    if (s_busy) {
        ESP_LOGW(TAG, "TTS busy, 拒绝新请求");
        return false;
    }
    if (!rb_buf) {
        tts_client_init();
        if (!rb_buf) {
            ESP_LOGE(TAG, "ringbuf not allocated! SPIRAM issue?");
            return false;
        }
    }

    s_busy = true;
    s_gen++;           /* 新世代 — 被打断的旧任务退出时不再触碰共享状态 */
    s_stop_req = false;
    rb_wr = 0;
    rb_rd = 0;
    rb_done = false;
    s_playback_task = NULL;

    tts_args_t *args = malloc(sizeof(tts_args_t));
    if (!args) { ESP_LOGE(TAG, "malloc args failed"); s_busy = false; return false; }
    args->text = strdup(text);
    if (!args->text) { ESP_LOGE(TAG, "strdup text failed"); free(args); s_busy = false; return false; }
    args->instruction = (instruction && instruction[0]) ? strdup(instruction) : NULL;

    /* Same priority: FreeRTOS round-robins, no producer/consumer starvation.
     * 栈放 PSRAM (CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY 已开):
     * 内部 RAM 碎片化时 "xTaskCreate failed: -1" 整段丢 TTS 的根治 */
    BaseType_t ret = xTaskCreateWithCaps(playback_task, "tts_play", 12288,
                                         NULL, 6, NULL, MALLOC_CAP_SPIRAM);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate play failed: %d", ret);
        free(args->text); free(args->instruction); free(args);
        s_busy = false;
        return false;
    }
    ret = xTaskCreateWithCaps(download_task, "tts_dl", 10240,
                              args, 6, NULL, MALLOC_CAP_SPIRAM);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate dl failed: %d", ret);
        rb_done = true;   /* playback drains & exits, 会清 s_busy */
        free(args->text); free(args->instruction); free(args);
        return false;
    }
    return true;
}

bool tts_speak(const char *text) { return tts_speak_internal(text, NULL); }
bool tts_speak_inst(const char *text, const char *instruction) {
    return tts_speak_internal(text, instruction);
}

bool tts_client_stop(void)
{
    if (!s_playback_task && !s_downloading && !s_busy) return false;
    ESP_LOGI(TAG, "TTS stop");
    s_stop_req = true;
    if (s_playback_task) xTaskNotifyGive(s_playback_task);
    return true;
}

bool tts_client_interrupt_speak(const char *text)
{
    if (!text || !text[0]) return false;
    if (!s_api_mutex) return false;
    if (xSemaphoreTake(s_api_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "API mutex busy");
        return false;
    }
    if (s_busy || s_playback_task || s_downloading) {
        ESP_LOGI(TAG, "打断当前播放");
        s_stop_req = true;
        if (s_playback_task) xTaskNotifyGive(s_playback_task);
        /* 协作式退出: 下载 ≤200ms (recv 超时), 播放 ≤200ms 静音冲刷 */
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(600);
        while ((s_playback_task || s_downloading) &&
               xTaskGetTickCount() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (s_playback_task || s_downloading) {
            /* 正常路径永不触发 — 兜底强杀防死锁 */
            ESP_LOGE(TAG, "旧任务未退出 — 强杀兜底");
            if (s_playback_task) { vTaskDelete(s_playback_task); s_playback_task = NULL; }
            s_downloading = false;  /* 旧 download 靠世代号自清理 */
        }
    }
    s_stop_req = false;
    bool ok = tts_speak_internal(text, NULL);
    if (!ok) {
        /* 内部 RAM 瞬时碎片化 — 等 300ms 重试一次 */
        ESP_LOGW(TAG, "首次创建失败, 300ms 后重试");
        vTaskDelay(pdMS_TO_TICKS(300));
        ok = tts_speak_internal(text, NULL);
    }
    xSemaphoreGive(s_api_mutex);
    return ok;
}
