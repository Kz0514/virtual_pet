/**
 * @file tts_client.c
 * @brief TTS 播放 — WS 流式 (主) + chunked HTTP 下载 (兜底)
 *
 * 架构:
 *   WS 音频流 (主): 二进制 PCM 逐片直喂 → 帧队列 → 搬运任务 → 单槽环 →
 *   播放任务整块写入 (I2S DMA 节拍)。download/playback 链为下载兜底路径。
 *   服务端把 LLM 回复按句子切分经 chat_text 帧下发, 固件逐句入队 —
 *   播放任务按链消费, 句间缝隙 ≈ 0 (下一句的 DNS/连接/TTFB 藏在语音后面)。
 *   chat_done 携带 tts_done 标记时不再整段 TTS, 防重复朗读。
 *
 * 环形缓冲: 单槽 512KB SPSC, lock-free — wr only by producer, rd only by consumer.
 * Unsigned 32-bit subtraction wr-rd always correct for byte count (max gap << 2^32).
 */
#include "tts_client.h"
#include "server_config.h"
#include "api_client.h"
#include "es8311_drv.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>

static const char *TAG = "tts";

/* ── 环形缓冲 (SPSC lock-free) ──
 * 单槽 512KB ≈ 5.5s @48kHz mono16, power of 2。下载路径仅兜底整段单句,
 * 双槽预下载 (播 A 下 B) 为死代码 — 单槽省 512KB PSRAM。
 * rb_put 满等 120s 背压. */
#define RB_SLOT_BYTES (512 * 1024)
#define RB_SLOTS 2 /* 数组尺寸保留 — 逻辑恒用槽 0 */

static uint8_t *rb_buf;                 /* SPIRAM 单槽 512KB (下载仅兜底整段单句) */
static volatile uint32_t rb_wr[RB_SLOTS]; /* producer index (bytes), monotonic, overflow OK */
static volatile uint32_t rb_rd[RB_SLOTS]; /* consumer index (bytes) */
static volatile bool rb_done[RB_SLOTS];   /* 本槽下载完成 (dl 退出时置位) */
static TaskHandle_t s_playback_task;    /* for task notification */
bool tts_client_is_playing(void) { return s_playback_task != NULL; }

/* 全周期忙碌标志 — 链式播放期间保持, 防止并发 TTS 请求污染缓冲 */
static volatile bool s_busy = false;
static volatile int s_dl_active = 0;    /* 活跃下载任务数 (诊断) */
static volatile bool s_slot_busy[RB_SLOTS]; /* 槽被下载任务占用 — dl_spawn 等待旧
                                             * 任务退出后再重置, 防双写混叠 */
static volatile uint32_t s_playback_start_tick = 0;

/* 播放期 PM 锁 — 轻睡冻结 I2S DMA 会破音; 持锁期间 esp_pm 完全禁止
 * 轻睡, 播放结束释放 */
static esp_pm_lock_handle_t s_pm_lock = NULL;

/* ── 协作式中断 (不杀任务) ── */
static volatile bool s_stop_req = false;     /* 停止请求 — 任务轮询 */
static volatile uint32_t s_gen = 0;          /* 世代号 — 每次新会话递增 */
static SemaphoreHandle_t s_api_mutex = NULL; /* 串行化 stop/speak/interrupt 入口 */

/* ── WS 音频流模式 ──
 * 服务器 LLM 流式期间 streaming_call 增量合成, 音频经 WS 二进制帧直推
 * 设备入环直播: audio_start → 起播放链 (预冲等音频); 二进制帧 → 入环;
 * audio_end → rb_done (排空收尾). 无 POST 无下载任务, 播放任务复用,
 * 顶部以 s_ws_mode 区分 (不再 q_pop 文本). */
static volatile bool s_ws_mode = false;

/* ── WS 帧队列 + 搬运任务 (背压闭环) ──
 * 组件任务不能阻塞 (卡 ping/pong → 断连) — ws_feed 只把帧零阻塞写入
 * 本队列; 独立搬运任务 ws_relay 取帧 → rb_put 满等阻塞 → 环满时 TCP
 * 窗口收紧 → 服务器 send_bytes 挂起, 速率恒 = 播放消费速率, 零估算
 * 零丢块 (除播放真实停滞 > 缓冲 10.6s). 双缓冲吸收合成突发与网络抖动. */
#define WS_Q_BYTES (256 * 1024) /* 帧队列 256KB + 环 512KB = 2.7s 存量,
                                   覆盖句间合成间隙 (省下更大 PSRAM 预算) */
#define WS_RELAY_BUF (9600 * 2) /* 播放整块 200ms, 与排空写纪律一致 */
static uint8_t *s_ws_q = NULL;          /* SPIRAM 帧队列环 */
static volatile uint32_t s_ws_q_wr = 0, s_ws_q_rd = 0; /* SPSC 指针 */
static volatile bool s_ws_q_done = false; /* audio_end 置位 — 搬完残余退出 */
static uint8_t *s_relay_buf = NULL;     /* 搬运中转块 */
static TaskHandle_t s_ws_relay = NULL;
static volatile uint32_t s_ws_relay_gen = 0; /* rb_put 世代守卫 */

/* ── 句子文本队列 (chat_text 帧 → 链式播放) ──
 * 单生产者 (WS 任务) 单消费者 (playback 任务), 互斥保护。
 * 满则丢新句 (极端场景; 正常每句 <100B, 8 句远超一轮回复)。
 * 动态条目 (PSRAM): 256B 硬上限会截断超长句 → 语音缺失/与文字错位,
 * 条目必须按实际句长分配 */
#define TTS_Q_LEN 8

static char *s_q[TTS_Q_LEN]; /* 条目 PSRAM, 所有权随出队转移给播放任务 */
static int s_q_head = 0, s_q_tail = 0;
static SemaphoreHandle_t s_q_mutex = NULL;

/* 重复句诊断: 最近一次取句的文本快照 — 播放任务再次取出相同文本即
 * 上游重发/重排 (WS 重连补发/LLM 重生成)。取句点比对打 WARN, 不跳过 —
 * 误跳 = 语音缺失, 比重复更糟 */
static char s_last_queued[64];

static int q_count(void) { return (s_q_tail - s_q_head + TTS_Q_LEN) % TTS_Q_LEN; }
static bool q_empty(void) { return s_q_head == s_q_tail; }

static void q_push(const char *text)
{
    if (!s_q_mutex) return;
    if (xSemaphoreTake(s_q_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    if (q_count() < TTS_Q_LEN - 1) {
        size_t len = strlen(text);
        char *p = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
        if (p) {
            memcpy(p, text, len + 1);
            s_q[s_q_tail] = p;
            s_q_tail = (s_q_tail + 1) % TTS_Q_LEN;
        }
    }
    xSemaphoreGive(s_q_mutex);
}

/* 取句 — 返回队列条目, 所有权转移给调用方 (用后 free), 无句返回 NULL */
/* 取句后记录快照; 与上次相同打 WARN (重复句诊断) */
static void tts_note_queued(const char *text)
{
    if (s_last_queued[0] && strcmp(s_last_queued, text) == 0)
        ESP_LOGW(TAG, "疑似重复句: '%.60s' — 上游重发?", text);
    size_t n = strlen(text);
    if (n >= sizeof(s_last_queued)) n = sizeof(s_last_queued) - 1;
    memcpy(s_last_queued, text, n);
    s_last_queued[n] = '\0';
}

static char *q_pop(void)
{
    char *out = NULL;
    if (!s_q_mutex) return NULL;
    if (xSemaphoreTake(s_q_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return NULL;
    if (!q_empty()) {
        out = s_q[s_q_head];
        s_q_head = (s_q_head + 1) % TTS_Q_LEN;
    }
    xSemaphoreGive(s_q_mutex);
    return out;
}

static void q_clear(void)
{
    if (!s_q_mutex) return;
    if (xSemaphoreTake(s_q_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    while (s_q_head != s_q_tail) {
        heap_caps_free(s_q[s_q_head]);
        s_q_head = (s_q_head + 1) % TTS_Q_LEN;
    }
    xSemaphoreGive(s_q_mutex);
}

bool tts_client_queue_empty(void) { return q_empty(); }

bool tts_client_is_downloading(void) { return s_dl_active > 0; }
bool tts_client_is_busy(void) { return s_busy; }

/* 播放已开始的毫秒数 (0=未开始) — 供文字同步使用 */
uint32_t tts_client_get_playback_ms(void)
{
    if (!s_playback_task || s_playback_start_tick == 0) return 0;
    return (xTaskGetTickCount() - s_playback_start_tick) * portTICK_PERIOD_MS;
}

static inline uint8_t *rb_slot(int s) { return rb_buf; } /* 单槽环形 */
static inline uint32_t rb_avail(int s) { return rb_wr[s] - rb_rd[s]; }
static inline uint32_t rb_free(int s) { return RB_SLOT_BYTES - (rb_wr[s] - rb_rd[s]); }

/* Called by download task; blocks until all bytes written. 返回 true=全部
 * 写入; false=槽满放弃 (rb_done 已置位, 播放侧排空已写入部分后换句).
 * Notifies playback task after adding data.
 * 世代守卫: 被打断的旧 download 任务不得再向新会话的缓冲写数据.
 * 满等 120s 判定播放侧真死 (es8311 阻塞/任务崩溃) — 置 done 放弃本句
 * 让链自愈; 网络欠载时播放侧泵静音等数据, 恢复窗口可达数十秒, 120s
 * 足够宽. 满等期间 recv 停摆, 数据堆在 TCP 缓冲 (可靠传输, 播放消费后
 * 流入), 8s 无数据看门狗不触发 (recv 未在跑) */
static bool rb_put(int slot, uint32_t gen, const uint8_t *data, uint32_t len)
{
    if (gen != s_gen) return true;
    TickType_t stall_t = xTaskGetTickCount();
    while (len > 0) {
        uint32_t free = rb_free(slot);
        if (free == 0) {
            if (xTaskGetTickCount() - stall_t > pdMS_TO_TICKS(120000)) {
                ESP_LOGW(TAG, "rb_put 槽满 120s — 播放侧疑死, 放弃本句 (slot %d)", slot);
                rb_done[slot] = true;
                if (s_playback_task) xTaskNotifyGive(s_playback_task);
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        stall_t = xTaskGetTickCount(); /* 有空间写入即重置 — 只对连续满计超时 */
        uint32_t n = (len < free) ? len : free;
        uint32_t pos = rb_wr[slot] & (RB_SLOT_BYTES - 1);
        uint32_t first = RB_SLOT_BYTES - pos;
        if (n <= first) {
            memcpy(rb_slot(slot) + pos, data, n);
        } else {
            memcpy(rb_slot(slot) + pos, data, first);
            memcpy(rb_slot(slot), data + first, n - first);
        }
        rb_wr[slot] += n;
        data += n;
        len -= n;
    }
    /* Wake playback task — data arrived */
    if (s_playback_task) xTaskNotifyGive(s_playback_task);
    return true;
}

/* Non-blocking: read up to max_len, return bytes read (may be 0) */
static uint32_t rb_try_get(int slot, uint8_t *data, uint32_t max_len)
{
    uint32_t avail = rb_avail(slot);
    if (avail == 0) return 0;
    uint32_t n = (max_len < avail) ? max_len : avail;
    uint32_t pos = rb_rd[slot] & (RB_SLOT_BYTES - 1);
    uint32_t first = RB_SLOT_BYTES - pos;
    if (n <= first) {
        memcpy(data, rb_slot(slot) + pos, n);
    } else {
        memcpy(data, rb_slot(slot) + pos, first);
        memcpy(data + first, rb_slot(slot), n - first);
    }
    rb_rd[slot] += n;
    return n;
}

/* ── Download task ── */
typedef struct {
    char *text;
    int slot;      /* 目标槽 */
    uint32_t gen;  /* 世代 — 打断后旧任务不再触碰槽 */
} tts_args_t;

/* ── chunked transfer 解码状态机 ── */
typedef enum { CH_SIZE,
               CH_SIZE_LF,
               CH_DATA,
               CH_DATA_CR,
               CH_DATA_LF,
               CH_DONE } ch_state_t;

typedef struct {
    ch_state_t st;
    uint32_t remaining; /* CH_DATA 剩余字节 */
    uint32_t size_val;  /* 当前 chunk 大小 (十六进制) */
    uint32_t gen;       /* 世代守卫 */
} chunk_parser_t;

static void cp_reset(chunk_parser_t *p, uint32_t gen)
{
    p->st = CH_SIZE;
    p->remaining = 0;
    p->size_val = 0;
    p->gen = gen;
}

/* 喂入原始字节, 只把 chunk 数据写入环形缓冲 (宽容解析, 容忍 \r 丢失).
 * 返回 false = rb_put 槽满放弃 (本句已结束, 调用方停止喂入) */
static bool cp_feed(chunk_parser_t *p, int slot, const uint8_t *d, uint32_t len)
{
    uint32_t i = 0;
    while (i < len && p->st != CH_DONE) {
        uint8_t c = d[i];
        switch (p->st) {
        case CH_SIZE: /* 十六进制大小行 */
            if (c >= '0' && c <= '9')
                p->size_val = p->size_val * 16 + (c - '0');
            else if (c >= 'a' && c <= 'f')
                p->size_val = p->size_val * 16 + (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                p->size_val = p->size_val * 16 + (c - 'A' + 10);
            else if (c == '\r')
                p->st = CH_SIZE_LF;
            i++;
            break;
        case CH_SIZE_LF:
            i++;
            if (p->size_val == 0) {
                p->st = CH_DONE;
                break;
            }
            p->st = CH_DATA;
            p->remaining = p->size_val;
            break;
        case CH_DATA: {
            uint32_t avail = len - i;
            uint32_t n = (avail < p->remaining) ? avail : p->remaining;
            if (n > 0) {
                if (!rb_put(slot, p->gen, d + i, n)) return false;
                i += n;
                p->remaining -= n;
            }
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
            p->st = CH_SIZE;
            p->size_val = 0;
            break;
        case CH_DONE:
            return true;
        }
    }
    return true;
}

/* download 任务统一退出 (永不返回) — 世代守卫: 旧任务不再触碰共享状态.
 * rb_done[slot] 置位 — 播放侧预冲等待/排空据此终止 */
static void dl_exit(tts_args_t *args, int sock)
{
    if (sock >= 0) close(sock);
    if (args->gen == s_gen) {
        rb_done[args->slot] = true;
    }
    s_slot_busy[args->slot] = false; /* 无条件清 — 任务退出即不再写槽 */
    if (s_dl_active > 0) s_dl_active--;
    if (s_playback_task) xTaskNotifyGive(s_playback_task);
    if (args) {
        free(args->text);
        free(args);
    }
    vTaskDelete(NULL);
}

/* 单次 TTS 下载尝试 — 成功返回 true (sock 已关闭)。
 * *out_slot_full: 槽满放弃 (播放侧疑死) — 非网络问题, 重试只会重写同槽
 * 造成数据混叠, 调用方据此不重试。
 * 所有失败路径打日志 — 静默失败 (头部超时即退/send 未检) 会让网络瞬断
 * 时的 TTS 丢失无任何痕迹 */
static bool tts_dl_attempt(tts_args_t *args, bool *out_slot_full)
{
    *out_slot_full = false;
    const char *text = args->text;
    const char *token = api_client_get_token();
    if (!token || !token[0]) {
        ESP_LOGE(TAG, "TTS: no token!");
        return false;
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

    /* HTTP/1.0 POST */
    static char req[3584];
    snprintf(req, sizeof(req),
             "POST /api/v1/tts/synthesize-stream?text=%s"
             "&token=%s"
             " HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
             enc, token, SERVER_HOST);

    struct addrinfo h = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *r;
    char ps[8];
    snprintf(ps, sizeof(ps), "%d", SERVER_PORT);
    if (getaddrinfo(SERVER_HOST, ps, &h, &r) || !r) {
        ESP_LOGE(TAG, "TTS: DNS fail");
        return false;
    }
    int sock = socket(r->ai_family, r->ai_socktype, 0);
    if (sock < 0 || connect(sock, r->ai_addr, r->ai_addrlen) < 0) {
        ESP_LOGE(TAG, "TTS: connect fail (errno %d)", errno);
        freeaddrinfo(r);
        if (sock >= 0) close(sock);
        return false;
    }
    freeaddrinfo(r);

    /* 200ms 收超时 — 停止标志能被及时轮询 */
    struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ssize_t sn = send(sock, req, strlen(req), 0);
    if (sn < 0 || sn != (ssize_t)strlen(req)) {
        ESP_LOGE(TAG, "TTS: send 失败 (%d/%d, errno %d)",
                 (int)sn, (int)strlen(req), errno);
        close(sock);
        return false;
    }

    /* 头部 200ms/轮轮询, 总超时 10s — 网络瞬断与服务端抖动通常在数秒内
     * 恢复; EOF/无头/超时均打日志 */
    static char t[4096];
    int n = 0;
    TickType_t hdr_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
    while (n < (int)sizeof(t) - 1) {
        int r = recv(sock, t + n, sizeof(t) - 1 - n, 0);
        if (r > 0) {
            n += r;
            t[n] = 0;
            if (strstr(t, "\r\n\r\n") || strstr(t, "\n\n")) break; /* 头部完整 */
        } else if (r == 0) {
            ESP_LOGE(TAG, "TTS: 连接被关闭 — HTTP 头未到 (n=%d)", n);
            close(sock);
            return false;
        } else {
            if (s_stop_req) {
                close(sock);
                return false;
            } /* 用户打断 */
            if (xTaskGetTickCount() >= hdr_deadline) {
                ESP_LOGE(TAG, "TTS: 头部 10s 超时 (errno %d) — 网络不可达", errno);
                close(sock);
                return false;
            }
            /* SO_RCVTIMEO 超时 — 继续轮询 */
        }
    }
    char *bd = strstr(t, "\r\n\r\n");
    if (!bd) bd = strstr(t, "\n\n");
    if (!bd) {
        ESP_LOGE(TAG, "TTS: 无 HTTP 头分隔符 (n=%d)", n);
        close(sock);
        return false;
    }
    if (!strstr(t, " 200 ")) {
        ESP_LOGE(TAG, "TTS HTTP 非200: %.12s", t);
        close(sock);
        return false;
    }
    bd += (bd[0] == '\r') ? 4 : 2; /* skip past header boundary */
    int leftover = n - (bd - t);

    TickType_t t_start = xTaskGetTickCount();
    uint32_t dl_bytes = leftover > 0 ? leftover : 0; /* 速率诊断 */
    uint32_t dl_last = 0;
    TickType_t dl_last_t = t_start;

    /* chunked 解码 — 边收边解边入环, 流式开播 */
    chunk_parser_t cp;
    cp_reset(&cp, args->gen);
    if (leftover > 0) {
        if (!cp_feed(&cp, args->slot, (uint8_t *)bd, leftover)) {
            ESP_LOGW(TAG, "TTS: 槽满放弃 (头部残留) — 本句结束");
            close(sock);
            *out_slot_full = true;
            return false;
        }
        if (s_playback_task) xTaskNotifyGive(s_playback_task);
    }

    static uint8_t buf[4096];
    /* 无数据 8s 看门狗 — 服务端挂死 (不回应也不关连接) 时本任务必须退出,
     * 否则链式播放卡死在预冲等待 (recv 超时循环本身不会退出) */
    TickType_t last_data_t = xTaskGetTickCount();
    while (cp.st != CH_DONE) {
        n = recv(sock, buf, sizeof(buf), 0);
        if (n < 0) {
            /* SO_RCVTIMEO 超时 — 轮询停止标志后继续等 */
            if (s_stop_req) break;
            if (xTaskGetTickCount() - last_data_t > pdMS_TO_TICKS(8000)) {
                ESP_LOGW(TAG, "TTS: 下载 8s 无数据 — 放弃 (errno %d)", errno);
                break;
            }
            continue;
        }
        if (n == 0) {
            ESP_LOGW(TAG, "TTS: 流式下载提前断开 (总 %dKB, %dms)",
                     (int)(dl_bytes / 1024),
                     (int)((xTaskGetTickCount() - t_start) * portTICK_PERIOD_MS));
            break; /* 连接关闭 */
        }
        last_data_t = xTaskGetTickCount();
        dl_bytes += n;
        if (!cp_feed(&cp, args->slot, buf, n)) {
            ESP_LOGW(TAG, "TTS: 槽满放弃 (总 %dKB) — 本句结束",
                     (int)(dl_bytes / 1024));
            close(sock);
            *out_slot_full = true;
            return false;
        }
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
    close(sock);
    return true;
}

static void download_task(void *pv)
{
    tts_args_t *args = (tts_args_t *)pv;
    if (s_dl_active >= 0) s_dl_active++;
    if (!args || !args->text) {
        dl_exit(args, -1);
        return;
    }
    s_slot_busy[args->slot] = true; /* 占用槽 — dl_spawn 据此防双写 */
    ESP_LOGI(TAG, "TTS dl start (slot %d): '%.60s'", args->slot, args->text);
    /* 失败重试一次 (1s 后) — 网络瞬断场景第二次大概率恢复; 重试期间
     * rb_done 不置位, playback 预冲继续等待 (世代守卫保证安全).
     * 槽满放弃不重试 — 播放侧疑死, 重试会重写同槽造成数据混叠 */
    bool ok = false, slot_full = false;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt > 0) {
            if (s_stop_req) break;
            ESP_LOGW(TAG, "TTS 下载失败 — 1s 后重试");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        ok = tts_dl_attempt(args, &slot_full);
        if (ok || slot_full || s_stop_req) break;
    }
    if (!ok && !slot_full && !s_stop_req)
        ESP_LOGE(TAG, "TTS: 下载重试均失败 — 本句放弃");
    dl_exit(args, -1);
}

/* 起一次下载: 重置槽计数器 + 生成 dl 任务. 成功返回 true.
 * 文本所有权移交 dl 任务 (退出时释放); 失败调用方自行 free
 * 槽占用保护: 旧下载任务未退出时重置槽 → 新旧任务写同一槽, 各自维护
 * wr 指针 → 数据混叠 (垃圾 PCM 尖锐噪音)。等旧任务退出 (最多 3s, 其
 * 8s 看门狗保证有限等待); 仍占用则放弃本句 */
static bool dl_spawn(char *text, int slot, uint32_t gen)
{
    for (int i = 0; i < 60 && s_slot_busy[slot]; i++)
        vTaskDelay(pdMS_TO_TICKS(50));
    if (s_slot_busy[slot]) {
        ESP_LOGE(TAG, "槽 %d 仍被旧任务占用 (3s) — 放弃本句", slot);
        return false;
    }
    tts_args_t *a = malloc(sizeof(tts_args_t));
    if (!a) {
        ESP_LOGE(TAG, "malloc args failed");
        return false;
    }
    a->text = text;
    a->slot = slot;
    a->gen = gen;
    rb_wr[slot] = 0;
    rb_rd[slot] = 0;
    rb_done[slot] = false;
    BaseType_t ret = xTaskCreateWithCaps(download_task, "tts_dl", 10240,
                                         a, 6, NULL, MALLOC_CAP_SPIRAM);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate dl failed: %d", ret);
        free(a->text);
        free(a);
        rb_done[slot] = true; /* 无下载 — 免播放侧误等 */
        return false;
    }
    return true;
}

/* ── Playback task: 链式播放驱动 ──
 * 单任务跑完整条回复链: 起下载 → 预冲 → 起下一句 → 排空 → 换槽.
 * 链结束 (队列空) 或被打断时退出并清理.
 * 预冲 0.5s 足以吸收短抖动; underruns 计数监控欠冲。 */
#define PLAY_CHUNK_SAMPLES 9600 /* 200ms @48kHz — 整 desc 写纪律:
                                 * esp_driver_i2s TX desc 环 dw0.len 固定全长度,
                                 * DMA 播满 200ms 才移下一 desc; 半块写 = 播到
                                 * 未填部分 = 旧内容 (复播句首根因) */
#define MIN_START_SAMPLES 24000 /* 0.5s @48kHz — 首句 burst 后环仍有秒级
                                 * 缓冲, 开播更快 */

/* 静音泵 — 整 desc 满块写, 源用 PSRAM chunk 清零 (播放期任何 flash
 * 操作会冻结 flash cache → DMA 从 flash 直读源挂起/读错, 故源必须
 * PSRAM; chunk 只在泵调用期间使用, 内容无意义, 零额外分配) */
static inline void pump_silence(int16_t *chunk)
{
    memset(chunk, 0, PLAY_CHUNK_SAMPLES * 2);
    es8311_drv_write(chunk, 9600);
}

static void playback_task(void *pv)
{
    s_playback_task = xTaskGetCurrentTaskHandle();
    uint32_t my_gen = s_gen;
    /* 播放期禁轻睡 (I2S DMA 冻结会破音) — 各退出路径与 interrupt 强杀处释放;
     * 同时占用 I2S 通道 (电源管理 v1) */
    if (s_pm_lock) esp_pm_lock_acquire(s_pm_lock);
    es8311_drv_hold();

    int16_t *chunk = heap_caps_malloc(PLAY_CHUNK_SAMPLES * 2, MALLOC_CAP_SPIRAM);
    if (!chunk) {
        if (my_gen == s_gen) {
            s_playback_task = NULL;
            s_busy = false;
            s_ws_mode = false; /* WS 链结束复位 */
            s_playback_start_tick = 0;
        }
        es8311_drv_release();
        if (s_pm_lock) esp_pm_lock_release(s_pm_lock);
        vTaskDelete(NULL);
        return;
    }
    esp_codec_dev_handle_t dac = es8311_get_dac_handle();

    int slot = 0; /* 单槽环形 */
    int total_played = 0;
    int underruns = 0;
    bool stopped = false;
    bool dac_ready = false;
    TickType_t t_start = xTaskGetTickCount();
    s_playback_start_tick = t_start;

    /* 静音泵缓冲: 任何等待期 (预冲/欠载/链尾) 写静音保持 DMA 流 —
     * ★ 必须整 desc 写 (9600 samples = 200ms 满块): i2s_common.c 的 TX 环
     * 是硬件级循环链 (desc[i]->next = desc[i+1], 末个回链 desc[0],
     * dw0.len 固定全长度), DMA 播满 200ms 才移下一 desc。小块只部分填充 →
     * DMA 播到未填部分 = 环内旧内容; 排空也须整块写 + 句尾静音补齐,
     * 环内任何时刻零残留, 泵仅兜底 */

    while (1) {
        if (s_stop_req) {
            stopped = true;
            break;
        }

        /* 本槽无内容 (链起点/失败跳过/晚到句) — 取一句文本并起下载;
         * WS 模式: 无文本队列, rb_done 由 audio_end 置位 */
        if (rb_done[slot] && rb_avail(slot) == 0) {
            if (s_ws_mode) {
                /* audio_end 已到但帧队列还有残余 — 搬运任务未搬完,
                 * 等帧队列搬完 (背压闭环) */
                if (s_ws_q && s_ws_q_rd != s_ws_q_wr) {
                    pump_silence(chunk);
                    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
                    continue;
                }
                break; /* WS 音频流结束 — 链结束 */
            }
            char *text = q_pop();
            if (!text) break; /* 队列空 — 链结束 */
            tts_note_queued(text);
            if (!dl_spawn(text, slot, my_gen)) {
                free(text);
                continue; /* 起下载失败 — 试下一句 */
            }
        }

        /* 预冲: 0.5s 音频或本句下载完成 (前一句播放期已预发起下载, 通常即刻就绪).
         * 下载模式 30s 兜底超时 (dl 自身还有 8s 无数据看门狗, 双保险);
         * WS 模式禁用看门狗 — LLM 停顿 (20s+) 期间合成器无输出是正常现象,
         * 播放链必须静音等待到 audio_end, 不得放弃 (误杀 → 后到音频全丢).
         * 等待期静音泵: 写静音保持 DMA 流 */
        TickType_t wait_start = xTaskGetTickCount();
        while (rb_avail(slot) < MIN_START_SAMPLES * 2 && !rb_done[slot]) {
            if (s_stop_req) {
                stopped = true;
                break;
            }
            if (!s_ws_mode &&
                xTaskGetTickCount() - wait_start > pdMS_TO_TICKS(30000)) {
                ESP_LOGE(TAG, "本句 30s 无数据 — 放弃本句");
                rb_done[slot] = true; /* 强制标记完成 — 不置位会再次落入
                                       * 排空死等, 队列里后续句子永不播放 */
                break;
            }
            pump_silence(chunk); /* 静音泵: 整 desc 满块 */
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        }
        if (stopped) break;
        if (rb_avail(slot) == 0 && rb_done[slot]) {
            ESP_LOGW(TAG, "本句空音频/失败 — 跳过");
            continue; /* 回顶部起下一句下载 */
        }

        /* 首句前: DAC 就绪. WS 模式: 预冲循环已泵静音清残留, 跳过
         * 2 块 pump (省起播前 400ms 静音 = "卡两下"); 链尾冲刷已保证
         * 环内零残留, 预冲泵即足矣 */
        if (!dac_ready) {
            es8311_drv_set_vol(100);
            if (dac) esp_codec_dev_write_reg(dac, 0x32, 0xCC);
            if (!s_ws_mode) {
                pump_silence(chunk);
                pump_silence(chunk);
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            dac_ready = true;
        }

        /* 排空本槽 — es8311_drv_write 阻塞即 DMA 节拍, 自然控速.
         * 下载模式 10s 无进展 (无数据可读且下载未完成) 判定下载侧已死 —
         * 强制换句, 防排空死等冻结整链. WS 模式: 看门狗禁用 — 播放节奏
         * = LLM 生成节奏, 停顿期无数据是正常现象, 静音泵等待到 audio_end */
        TickType_t drain_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
        while (1) {
            if (s_stop_req) {
                stopped = true;
                break;
            }
            /* 整 desc 写纪律: 只写 9600-sample 整块 — esp_driver_i2s TX desc 环
             * dw0.len 固定全长度 200ms, DMA 无论写入多少都播满; 半块写 =
             * 播到未填部分 = 环内旧内容 → "复播一段句首"。整块写 = 环内
             * 永不残留旧内容; 句尾不足一块用静音补齐 (≤200ms, 顺带恢复
             * 句间停顿) */
            uint32_t avail = rb_avail(slot);
            if (avail >= 2) {
                uint32_t to_read = avail;
                if (to_read > PLAY_CHUNK_SAMPLES * 2) to_read = PLAY_CHUNK_SAMPLES * 2;
                to_read &= ~1u; /* force even — never break a sample */
                if (to_read < PLAY_CHUNK_SAMPLES * 2 && !rb_done[slot])
                    to_read = 0; /* 未到整块且句未结束 — 不写半块, 等攒满 */
                if (to_read >= 2) {
                    drain_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
                    uint32_t got = rb_try_get(slot, (uint8_t *)chunk, to_read);
                    if (got >= 2) {
                        int samples = got / 2;
                        if (samples < PLAY_CHUNK_SAMPLES) {
                            /* 句尾不足一块 — 静音补齐整 desc, 环内无半块残留 */
                            memset((uint8_t *)chunk + got, 0,
                                   (PLAY_CHUNK_SAMPLES - samples) * 2);
                            samples = PLAY_CHUNK_SAMPLES;
                        }
                        es8311_drv_write(chunk, samples);
                        total_played += samples;
                    }
                } else {
                    /* 半块攒齐等待 (下载慢于播放) — 泵保持 DMA 流 */
                    if (!s_ws_mode && !rb_done[slot]) underruns++;
                    /* WS 模式: 禁用看门狗 — LLM 停顿期静音等待, 见预冲循环注释;
                     * underruns 同理不计 (停顿 = 设计行为) */
                    if (!s_ws_mode && xTaskGetTickCount() > drain_deadline) {
                        ESP_LOGW(TAG, "排空 10s 无进展 — 强制换句 (slot %d)", slot);
                        rb_done[slot] = true;
                        break;
                    }
                    pump_silence(chunk); /* 静音泵: 整 desc 满块 */
                    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
                }
            } else if (rb_done[slot] && avail == 0) {
                /* WS 模式: 帧队列残余未搬完 — 静音泵等搬运任务 */
                if (s_ws_mode && s_ws_q && s_ws_q_rd != s_ws_q_wr) {
                    pump_silence(chunk);
                    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
                } else {
                    break;
                }
            } else {
                /* Buffer low — wait for download to deliver more data */
                if (!s_ws_mode && !rb_done[slot]) underruns++;
                /* WS 模式: 禁用看门狗 — 静音等待到 audio_end */
                if (!s_ws_mode && xTaskGetTickCount() > drain_deadline) {
                    ESP_LOGW(TAG, "排空 10s 无进展 — 强制换句 (slot %d)", slot);
                    rb_done[slot] = true;
                    break;
                }
                pump_silence(chunk); /* 静音泵: 整 desc 满块 */
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
            }
        }
        if (stopped) break;

        /* 单槽: 无句中切换 — 多句队列时回顶部起下一句下载
         * (下载模式恒整段单句) */
    }

    /* 链结束 — 冲刷 I2S DMA 环为全静音: 6 × 200ms 整 desc = 全环覆盖,
     * 下链开头不会循环复播残留。一句没播过无需冲刷 */
    if (total_played > 0) {
        for (int i = 0; i < 6; i++)
            pump_silence(chunk);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

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
        s_ws_mode = false; /* WS 链收尾复位 — 后续 POST 走下载模式 */
        s_playback_start_tick = 0;
    }
    es8311_drv_release();
    if (s_pm_lock) esp_pm_lock_release(s_pm_lock);
    vTaskDelete(NULL);
}

/* ── Public API ── */

void tts_client_init(void)
{
    if (!s_api_mutex) s_api_mutex = xSemaphoreCreateMutex();
    if (!s_q_mutex) s_q_mutex = xSemaphoreCreateMutex();
    if (!s_pm_lock) {
        if (esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "tts_play", &s_pm_lock) != ESP_OK)
            ESP_LOGW(TAG, "PM 锁创建失败 — 播放期不禁轻睡 (有破音风险)");
    }
    /* 环 + WS 帧队列开机预分配 — 启动期 PSRAM 最空; ws_start 时动画帧等
     * 已占 PSRAM, 分配失败只能降级直接写环 (丢块) */
    if (!rb_buf) {
        rb_buf = heap_caps_malloc(RB_SLOT_BYTES, MALLOC_CAP_SPIRAM);
        if (rb_buf) {
            ESP_LOGI(TAG, "TTS streaming ready (单槽 %dKB, 队列 %d 条动态)",
                     (int)(RB_SLOT_BYTES / 1024), TTS_Q_LEN);
        } else {
            ESP_LOGE(TAG, "ringbuf alloc FAILED! SPIRAM available?");
        }
    }
    if (!s_ws_q) {
        s_ws_q = heap_caps_malloc(WS_Q_BYTES, MALLOC_CAP_SPIRAM);
        if (s_ws_q) {
            ESP_LOGI(TAG, "WS 帧队列 %dKB 预分配", (int)(WS_Q_BYTES / 1024));
        } else {
            ESP_LOGW(TAG, "WS 帧队列分配失败 — WS 模式降级直接写环");
        }
    }
    if (!s_relay_buf)
        s_relay_buf = heap_caps_malloc(WS_RELAY_BUF, MALLOC_CAP_SPIRAM);
}

/* 链起点: 重置单槽 + 起 playback (首句由 playback 自取, 避免 push/start 竞态).
 * 队列由调用方管理 (interrupt 清旧链残留) */
static bool tts_start_chain(void)
{
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
    /* 下载模式起点: 强制复位 s_ws_mode 防遗留 WS 态 (兜底 POST 竞态:
     * chat_done 先于播放收链到达) */
    s_ws_mode = false;

    s_busy = true;
    s_gen++; /* 新世代 — 被打断的旧任务退出时不再触碰共享状态 */
    s_stop_req = false;
    for (int i = 0; i < RB_SLOTS; i++) {
        rb_wr[i] = 0;
        rb_rd[i] = 0;
        rb_done[i] = true; /* 空槽 + 无下载 — playback 顶部据此起下载 */
    }
    s_playback_task = NULL;

    /* Same priority: FreeRTOS round-robins, no producer/consumer starvation.
     * 栈放 PSRAM (CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY 已开):
     * 内部 RAM 碎片化时 "xTaskCreate failed: -1" 整段丢 TTS 的根治 */
    BaseType_t ret = xTaskCreateWithCaps(playback_task, "tts_play", 12288,
                                         NULL, 6, NULL, MALLOC_CAP_SPIRAM);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate play failed: %d", ret);
        s_busy = false;
        return false;
    }
    return true;
}

/* chat_text 帧入口 (P1 流式): 入队; 链空闲则起链.
 * 竞态安全: push 先于 start — 若 playback 已取空退出, s_busy 为 false,
 * 此处再起一条新链取回该句, 文本不会丢也不会重播 */
bool tts_speak_queue(const char *text)
{
    if (!text || !text[0]) return false;
    q_push(text);
    if (!s_busy) return tts_start_chain();
    return true;
}

bool tts_speak(const char *text) { return tts_speak_queue(text); }

bool tts_speak_inst(const char *text, const char *instruction)
{
    /* [inst:] 自然语言语音控制已禁用 (不同输入大幅改变音色不稳定) —
     * 忽略 instruction, 走与 tts_speak 相同的队列路径 */
    return tts_speak_queue(text);
}

bool tts_client_stop(void)
{
    if (!s_busy) return false;
    ESP_LOGI(TAG, "TTS stop");
    s_stop_req = true;
    q_clear(); /* 链残留句子丢弃 — 后续新回复不得重播 */
    if (s_playback_task) xTaskNotifyGive(s_playback_task);
    return true;
}

/* 打断当前播放链 (协作式, 兜底强杀) — 返回打断是否完成 (s_busy 已清).
 * 调用方需持有 s_api_mutex (ws_start / interrupt_speak 入口串行化) */
static bool _interrupt_chain(void)
{
    if (!s_busy && !s_playback_task) return true;
    ESP_LOGI(TAG, "打断当前播放");
    s_stop_req = true;
    if (s_playback_task) xTaskNotifyGive(s_playback_task);
    /* 协作式退出: 下载 ≤200ms (recv 超时), 播放 ≤200ms 静音冲刷 */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(600);
    while ((s_playback_task || s_busy) && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_playback_task) {
        /* 正常路径永不触发 — 兜底强杀防死锁 */
        ESP_LOGE(TAG, "旧任务未退出 — 强杀兜底");
        /* 任务还活着必持着 PM 锁 + I2S 占用 (acquire/hold 在任务开头) — 先释放防锁泄漏 */
        if (s_pm_lock) esp_pm_lock_release(s_pm_lock);
        es8311_drv_release();
        vTaskDelete(s_playback_task);
        s_playback_task = NULL;
        s_busy = false; /* 被杀任务不会自行清理 */
        s_ws_mode = false;
    }
    /* 搬运任务同理 — 可能阻塞在 rb_put 满等, 杀后 SPSC 指针未更新的
     * 半帧由下次会话重读, 无残留 */
    if (s_ws_relay) {
        vTaskDelete(s_ws_relay);
        s_ws_relay = NULL;
    }
    return true;
}

/* ── WS 音频流模式 API ──
 * audio_start: 起播放链 (预冲等 WS 音频). 打断旧链后重置槽 0.
 * ws_feed:     WS 任务上下文调用 (esp_websocket_client 组件任务!) —
 *              必须零阻塞: 写帧队列 (满 = 播放停滞, 丢块计数), 有空间
 *              必立即写. 帧队列 → 搬运任务 → 环 (rb_put 满等背压).
 * ws_end:      audio_end → rb_done 置位 + 帧队列 done, 播放排空收尾.
 * 播放任务复用 (s_ws_mode 区分), 无 POST 无下载任务. */

/* 搬运任务: 帧队列 → 播放环, rb_put 满等阻塞 = 背压点 (TCP 传导).
 * 退出: s_stop_req 或 audio_end (搬完残余). 世代守卫防写旧会话. */
static void ws_relay_task(void *arg)
{
    for (;;) {
        if (s_stop_req || (s_ws_q_done && s_ws_q_rd == s_ws_q_wr)) break;
        uint32_t avail = s_ws_q_wr - s_ws_q_rd;
        if (avail == 0) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        uint32_t len = avail > WS_RELAY_BUF ? WS_RELAY_BUF : avail;
        uint32_t pos = s_ws_q_rd & (WS_Q_BYTES - 1);
        uint32_t first = WS_Q_BYTES - pos;
        if (len <= first)
            memcpy(s_relay_buf, s_ws_q + pos, len);
        else {
            memcpy(s_relay_buf, s_ws_q + pos, first);
            memcpy(s_relay_buf + first, s_ws_q, len - first);
        }
        s_ws_q_rd += len; /* SPSC: 读指针仅本任务更新 */
        if (!rb_put(0, s_ws_relay_gen, s_relay_buf, len)) break; /* 槽放弃 */
    }
    s_ws_relay = NULL;
    vTaskDelete(NULL);
}

bool tts_client_ws_start(void)
{
    if (!rb_buf) {
        tts_client_init();
        if (!rb_buf) {
            ESP_LOGE(TAG, "ringbuf not allocated! SPIRAM issue?");
            return false;
        }
    }
    if (!s_api_mutex) return false;
    if (xSemaphoreTake(s_api_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "ws_start: API mutex busy");
        return false;
    }
    if (s_busy || s_playback_task)
        _interrupt_chain();
    s_busy = true;
    s_gen++;
    s_stop_req = false;
    s_ws_mode = true;
    /* 帧队列 + 搬运任务 (背压闭环) — 缓冲已开机预分配, 失败降级直接写环 */
    if (s_ws_q) {
        s_ws_q_wr = s_ws_q_rd = 0;
        s_ws_q_done = false;
        if (s_relay_buf && !s_ws_relay) {
            s_ws_relay_gen = s_gen;
            if (xTaskCreateWithCaps(ws_relay_task, "tts_relay", 3072, NULL, 5,
                                    &s_ws_relay, MALLOC_CAP_SPIRAM) != pdPASS) {
                ESP_LOGW(TAG, "搬运任务创建失败 — 降级直接写环");
                s_ws_relay = NULL;
            }
        }
    }
    for (int i = 0; i < RB_SLOTS; i++) {
        rb_wr[i] = 0;
        rb_rd[i] = 0;
        rb_done[i] = true;
    }
    rb_done[0] = false; /* 槽0 等 WS 音频 */
    s_playback_task = NULL;
    BaseType_t ret = xTaskCreateWithCaps(playback_task, "tts_play", 12288,
                                         NULL, 6, NULL, MALLOC_CAP_SPIRAM);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate play failed: %d", ret);
        s_busy = false;
        s_ws_mode = false;
        xSemaphoreGive(s_api_mutex);
        return false;
    }
    xSemaphoreGive(s_api_mutex);
    return true;
}

bool tts_client_ws_feed(const uint8_t *data, uint32_t len)
{
    if (!s_busy || !s_ws_mode) return false;
    if (len == 0) return true;
    if (s_ws_q && s_ws_relay) {
        /* 帧队列满 = 搬运任务等环 = 播放停滞 — 丢块计数 (组件任务零阻塞) */
        if (WS_Q_BYTES - (s_ws_q_wr - s_ws_q_rd) < len) {
            static uint32_t ws_drops = 0;
            ws_drops++;
            if (ws_drops == 1 || (ws_drops & 0x3F) == 0)
                ESP_LOGW(TAG, "WS 帧队列满丢块 (累计 %u)", (unsigned)ws_drops);
            return true;
        }
        uint32_t pos = s_ws_q_wr & (WS_Q_BYTES - 1);
        uint32_t first = WS_Q_BYTES - pos;
        if (len <= first)
            memcpy(s_ws_q + pos, data, len);
        else {
            memcpy(s_ws_q + pos, data, first);
            memcpy(s_ws_q, data + first, len - first);
        }
        s_ws_q_wr += len; /* SPSC: 写指针仅组件任务更新 */
        return true;
    }
    /* 降级路径 (帧队列分配失败): 直接写环, 环满丢块 (旧行为) */
    if (rb_free(0) < len) {
        static uint32_t ws_drops = 0;
        ws_drops++;
        if (ws_drops == 1 || (ws_drops & 0x3F) == 0)
            ESP_LOGW(TAG, "WS 音频环满丢块 (累计 %u)", (unsigned)ws_drops);
        return true;
    }
    rb_put(0, s_gen, data, len);
    return true;
}

void tts_client_ws_end(void)
{
    if (!s_busy || !s_ws_mode) return;
    if (s_ws_q) s_ws_q_done = true; /* 搬运任务搬完残余后退出 */
    rb_done[0] = true;
    if (s_playback_task) xTaskNotifyGive(s_playback_task);
}

bool tts_client_is_ws_active(void) { return s_busy && s_ws_mode; }

bool tts_client_interrupt_speak(const char *text)
{
    if (!text || !text[0]) return false;
    if (!s_api_mutex) return false;
    if (xSemaphoreTake(s_api_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "API mutex busy");
        return false;
    }
    _interrupt_chain();
    s_stop_req = false;
    q_clear(); /* 旧链残留句子丢弃 */
    bool ok = tts_speak(text);
    if (!ok) {
        /* 内部 RAM 瞬时碎片化 — 等 300ms 重试一次 */
        ESP_LOGW(TAG, "首次创建失败, 300ms 后重试");
        vTaskDelay(pdMS_TO_TICKS(300));
        ok = tts_speak(text);
    }
    xSemaphoreGive(s_api_mutex);
    return ok;
}
