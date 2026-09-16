/**
 * @file data_writer.c
 * @brief /data 写入统一入口实现 — 见 data_writer.h
 *
 * 并发模型: 生产者 (任意任务上下文) 只往攒批缓冲 memcpy; 落盘由
 * data_writer_tick() 的调用方任务执行。锁只护住 memcpy 与游标, 不护
 * flash I/O — 落盘前先把本批搬进暂存区, 攒批缓冲立刻对生产者开放。
 * 缓冲/暂存均 PSRAM (直接 write 到 /data 已由 diary_sync 大文件走通),
 * 内部 RAM 占用为零。
 */
#include "data_writer.h"
#include "sensor_logger.h" /* sensor_logger_data_mounted — /data 可用性守卫 */
#include "tts_client.h"    /* tts_client_is_playing — 避让 flash 写冻结 */
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

static const char *TAG = "data_writer";

#define DW_HIGH_WATER_NUM 3         /* 攒到 3/4 缓冲提前落盘 */
#define DW_HIGH_WATER_DEN 4
#define DW_WSTAT_INTERVAL_MS 300000 /* # wstat 计数行 (5min) */

static const char POWER_LOG_HDR[] =
    "ms,mv,ma,soc,state,sleeps,rejects,winus,nxtalm,dexp,ddel,sof,tsf,prob,"
    "evalcnt,ret0,ret1,wincnt,wincore,winrem,winname,err,errcnt,wk,ph,tc,"
    "d0,d1,d2,d3,d4,d5,d6,d7,d8,d9,d10,d11\n";

static const char POWER_SEG_HDR[] =
    "type,ts_ms,up_ms,ma,mv,soc,src,wake_cnt,on_ms,seg_ms\n";

/* 覆盖模式: 每批即整份内容 (tasks.txt 要的是快照, 不是历史) */
typedef enum { DW_MODE_APPEND, DW_MODE_OVERWRITE } dw_mode_t;

typedef struct {
    const char *path;
    const char *hdr;    /* 空文件时先写 (NULL=无) */
    uint32_t max_bytes; /* 追加超限 → 原地截断重开 (0=不滚) */
    uint16_t buf_size;  /* 攒批缓冲 (PSRAM) */
    uint32_t flush_ms;  /* 本文件的攒批周期 — 必须长于数据点间隔才有降幅 */
    dw_mode_t mode;
    char *buf;
    uint16_t len;
    bool pending;
    int64_t due_ms;
} dw_entry_t;

/* 攒批周期的取值逻辑: 每文件独立计时, 一批只装"周期内到达的数据点" —
 * 周期短于数据点间隔时一批恰好一行, 擦除次数与直写**完全相同** (白攒)。
 * 所以周期必须跨越多个数据点, 否则这个文件进不进 manager 都不省擦除。
 * 反过来说, 周期越长越省, 代价 = 掉电丢失窗口变长 → 按"掉一行有多疼"
 * 分别取值, 不搞一刀切。 */
static dw_entry_t s_files[DW_FILE_N] = {
    /* 2s 一行: 一行就是一个时刻的电源快照, 单独看也有意义 → 30s 只攒 15 行,
     * 但它的量最大, 已经是成本的绝对主项 (估 8458 擦除/天) */
    [DW_POWER_LOG] = {
        .path = "/data/power_log.csv",
        .hdr = POWER_LOG_HDR,
        .max_bytes = 48 * 1024,
        .buf_size = 4096,
        .flush_ms = 30000,
        .mode = DW_MODE_APPEND,
    },
    /* 60s 一条 S 行, 单行只是聚合值 → 300s 攒 5 条才落一次盘
     * (2880 → 576 擦除/天)。丢 5 条聚合行不影响趋势判定。
     * W 行走 urgent 投递, 不受这个周期约束。
     * 缓冲 2048 而非 1024: 本文件同时承载 5min 一条的 # wstat 行 (~130B),
     * 而闸门关着时攒批刷不出去、只能靠缓冲留痕 —— 1024 只装得下 ~35 分钟,
     * 放大后才够回看一次低电挂起 */
    [DW_POWER_SEG] = {
        .path = "/data/power_seg.csv",
        .hdr = POWER_SEG_HDR,
        .max_bytes = 192 * 1024,
        .buf_size = 2048,
        .flush_ms = 300000,
        .mode = DW_MODE_APPEND,
    },
    /* 90s 一次快照, 30s 周期 = 一批一行 = 不省 — 故意不省: tasks.txt 的
     * 用处正是"出事那一刻任务卡在哪", 它越新越有用, 拿新鲜度换那点擦除
     * 不划算。覆盖模式故不滚 */
    [DW_TASKS] = {
        .path = "/data/tasks.txt",
        .hdr = NULL,
        .max_bytes = 0,
        .buf_size = 2048,
        .flush_ms = 30000,
        .mode = DW_MODE_OVERWRITE,
    },
};

static SemaphoreHandle_t s_lock = NULL;
static char *s_stage = NULL; /* 落盘暂存 (PSRAM), 落盘串行故全局共用一份 */
static bool s_ready = false;
static volatile bool s_gate_ok = true; /* fail-open, 与 memory_store_writes_safe 同源 */

/* 跨上下文: dump 单槽 */
static struct {
    bool used;
    dw_file_t f;
    dw_emit_fn emit;
    void *arg;
} s_stream;

/* 仅 tick 调用方任务访问 */
static uint32_t s_flush_cnt = 0;
static uint64_t s_bytes = 0;
static uint32_t s_drop = 0;
static int64_t s_wstat_ms = 0;
/* 冻结实测: 落盘期 flash 写会关双核 cache + IPC 停另一核, 时长只能量不能估。
 * last=本次, max=峰值, sum=累计 (sum/up_ms = 冻结占空比) */
static uint32_t s_stall_last_us = 0;
static uint32_t s_stall_max_us = 0;
static uint64_t s_stall_sum_us = 0;

static int64_t dw_now_ms(void) { return esp_timer_get_time() / 1000; }

/* 记一次落盘的冻结时长 (µs) */
static void dw_note_stall(int64_t t0)
{
    uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
    s_stall_last_us = dt;
    s_stall_sum_us += dt;
    if (dt > s_stall_max_us) s_stall_max_us = dt;
}

/* 计数行: 实测擦除量落在 CSV 注释里, 不新增列不新增文件 (解析按 '#' 跳)。
 * 本函数在一切闸门之前调用 — 闸门关 / 未挂载 / TTS 正是最需要这行的时候,
 * 放在早退之后等于永远看不见 (实测: 闸门恒关的板子跑几小时日志里连
 * data_writer 存在过都看不出来)。
 * hold 只进串口不进 CSV: 闸门关着时攒批刷不出去, 写进 CSV 是恒空死字段;
 * 但挂起期间的 wstat 仍会攒在缓冲里 — 闸门恢复后那一串 up_ms 递增而
 * flush 不变的注释行，正好是"被挂起多久"的事后记录。
 *
 * ⚠️ 落点是 **power_seg.csv 不是 power_log.csv**: 后者 48KB 滚动只留
 * ~16 分钟, 而看这行的人要的是"跑一天下来多少擦除" —— 写进 power_log
 * 等于样本还没读就被自己滚掉了。power_seg 192KB ≈ 46h 才留得住一天。
 * 因此 power_seg 的缓冲也相应放大 (见文件表的 buf_size 注释) */
static void dw_wstat_maybe(const char *hold)
{
    int64_t now = dw_now_ms();
    if (s_wstat_ms && now - s_wstat_ms < DW_WSTAT_INTERVAL_MS) return;
    s_wstat_ms = now;
    char line[192];
    int n = snprintf(line, sizeof(line),
                     "# wstat up_ms=%lld flush=%u bytes=%llu est_erase=%u drop=%u "
                     "stall_last=%u stall_max=%u stall_sum_ms=%llu\n",
                     (long long)now, (unsigned)s_flush_cnt,
                     (unsigned long long)s_bytes,
                     /* 每次 close = 数据扇区 + 目录项扇区各一擦 (追加必改长度
                      * → FA_MODIFIED → 目录项重写); 攒批超 4KB 再补多出的 */
                     (unsigned)(s_flush_cnt * 2u + (uint32_t)(s_bytes / 4096u)),
                     (unsigned)s_drop,
                     (unsigned)s_stall_last_us, (unsigned)s_stall_max_us,
                     (unsigned long long)(s_stall_sum_us / 1000));
    if (n > 0) data_writer_append(DW_POWER_SEG, line, n);
    /* 冻结占空比 = 累计冻结 / 运行时长 (万分之几) */
    unsigned duty = now > 0 ? (unsigned)(s_stall_sum_us / 1000 * 10000ULL / (uint64_t)now)
                            : 0;
    char hb[24];
    if (hold[0]) snprintf(hb, sizeof(hb), " [挂起:%s]", hold);
    else hb[0] = '\0';
    ESP_LOGI(TAG, "写入统计%s: flush=%u bytes=%llu est_erase=%u drop=%u | "
                  "冻结 末%ums 峰%ums 累计%llums 占空比万分之%u",
             hb,
             (unsigned)s_flush_cnt, (unsigned long long)s_bytes,
             (unsigned)(s_flush_cnt * 2u + (uint32_t)(s_bytes / 4096u)),
             (unsigned)s_drop,
             (unsigned)(s_stall_last_us / 1000), (unsigned)(s_stall_max_us / 1000),
             (unsigned long long)(s_stall_sum_us / 1000), duty);
}

/* 单次 open → 写全部 → close (擦除次数按批计, 而非按行计)。
 * 滚动检查沿用无状态式 (内存标志版被重启打断后会失效): 写入前查当前
 * 大小, 超限即原地截断复用簇链 — 不 remove: FAT 释放链更新丢失会累积
 * 孤儿簇 (chkdsk 曾一次找回 320KB)。注意裸 O_TRUNC 在 ESP-IDF FAT VFS
 * 是 no-op (fat_mode_conv 只在 O_CREAT|O_TRUNC 时给 FA_CREATE_ALWAYS) —
 * 必须带 O_CREAT, 否则截断永不生效 (1.0.283 实崩: 写满 724KB 分区) */
static bool dw_write_all(const dw_entry_t *e, const char *buf, int len)
{
    if (e->mode == DW_MODE_OVERWRITE) {
        /* 覆盖写。O_TRUNC 必须与 O_CREAT 同用: 裸 O_TRUNC 在 ESP-IDF FAT
         * VFS 是 no-op (fat_mode_conv 只在 O_CREAT|O_TRUNC 时给
         * FA_CREATE_ALWAYS) — 见下 */
        int fd = open(e->path, O_CREAT | O_TRUNC | O_WRONLY);
        if (fd < 0) return false;
        ssize_t w = write(fd, buf, (size_t)len);
        close(fd);
        return (w == (ssize_t)len);
    }
    if (e->max_bytes) {
        int fd = open(e->path, O_CREAT | O_APPEND | O_WRONLY);
        if (fd < 0) return false;
        if (lseek(fd, 0, SEEK_END) > (off_t)e->max_bytes) {
            close(fd);
            int t = open(e->path, O_CREAT | O_TRUNC | O_WRONLY);
            if (t < 0) return false;
            close(t);
        } else {
            close(fd);
        }
    }
    int fd = open(e->path, O_CREAT | O_APPEND | O_WRONLY);
    if (fd < 0) return false;
    if (e->hdr && lseek(fd, 0, SEEK_END) == 0)
        write(fd, e->hdr, strlen(e->hdr));
    ssize_t w = write(fd, buf, (size_t)len);
    close(fd);
    return (w == (ssize_t)len);
}

/* 落盘一批 (tick 调用方任务上下文, 串行) */
static void dw_flush(dw_file_t f)
{
    dw_entry_t *e = &s_files[f];
    uint16_t n = 0;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        n = e->len;
        if (n) memcpy(s_stage, e->buf, n);
        e->len = 0;
        e->pending = false;
        e->due_ms = 0;
        xSemaphoreGive(s_lock);
    }
    if (n) {
        int64_t t0 = esp_timer_get_time();
        bool ok = dw_write_all(e, s_stage, n);
        dw_note_stall(t0);
        if (ok) {
            s_flush_cnt++;
            s_bytes += n;
        } else {
            s_drop++;
            ESP_LOGW(TAG, "%s 落盘失败 — 本批 %u B 丢弃", e->path, (unsigned)n);
        }
    }

    /* FILE* 型 dump: 已按序接在攒批之后, 同一落盘时机写出 */
    bool do_stream = false;
    dw_emit_fn emit = NULL;
    void *arg = NULL;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        if (s_stream.used && s_stream.f == f) {
            do_stream = true;
            emit = s_stream.emit;
            arg = s_stream.arg;
            s_stream.used = false;
        }
        xSemaphoreGive(s_lock);
    }
    if (do_stream) {
        /* fopen 一次性 FILE+锁 (内部 RAM) — 调用方负责堆守卫 */
        int64_t t0 = esp_timer_get_time();
        FILE *fp = fopen(e->path, "a");
        if (fp) {
            emit(fp, arg);
            fclose(fp);
            dw_note_stall(t0);
            s_flush_cnt++;
        } else {
            s_drop++;
            ESP_LOGW(TAG, "%s dump 落盘失败 (内部堆不足?)", e->path);
        }
    }
}

void data_writer_tick(void)
{
    if (!s_ready) return;

    /* 挂起原因 — 统计行必须无条件可见 (见 dw_wstat_maybe 注释) */
    const char *hold = !s_gate_ok                      ? "闸门关"
                       : !sensor_logger_data_mounted() ? "未挂载"
                       : tts_client_is_playing()       ? "TTS"
                                                       : "";
    dw_wstat_maybe(hold);
    if (hold[0]) return;

    int64_t now = dw_now_ms();
    for (int i = 0; i < DW_FILE_N; i++) {
        dw_entry_t *e = &s_files[i];
        if (!e->path || !e->buf) continue;
        bool due;
        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            due = (s_stream.used && s_stream.f == (dw_file_t)i) ||
                  (e->pending && now >= e->due_ms);
            xSemaphoreGive(s_lock);
        } else {
            due = false;
        }
        if (due) dw_flush((dw_file_t)i);
    }
}

static bool dw_append_impl(dw_file_t f, const char *buf, int len, bool urgent)
{
    if (f >= DW_FILE_N || !buf || len <= 0 || !s_ready) return false;
    dw_entry_t *e = &s_files[f];
    if (!e->buf || len > (int)e->buf_size) return false;

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return false;
    if (e->mode == DW_MODE_OVERWRITE) {
        e->len = 0; /* 覆盖模式: 攒批期内多次投递只留最后一次 */
    } else {
        /* 缓冲满 → 丢最老的行: 最接近掉电的数据最要紧 */
        while ((int)e->len + len > (int)e->buf_size) {
            char *nl = memchr(e->buf, '\n', e->len);
            if (!nl) {
                e->len = 0;
                break;
            }
            size_t d = (size_t)(nl - e->buf) + 1;
            memmove(e->buf, e->buf + d, (size_t)e->len - d);
            e->len -= (uint16_t)d;
            s_drop++;
        }
    }
    memcpy(e->buf + e->len, buf, (size_t)len);
    e->len += (uint16_t)len;
    if (!e->pending) {
        e->pending = true;
        e->due_ms = dw_now_ms() + e->flush_ms;
    }
    if (urgent ||
        e->len >= (int)(e->buf_size / DW_HIGH_WATER_DEN * DW_HIGH_WATER_NUM))
        e->due_ms = dw_now_ms(); /* 攒满前先吐, 避免丢行 */
    xSemaphoreGive(s_lock);
    return true;
}

bool data_writer_append(dw_file_t f, const char *buf, int len)
{
    return dw_append_impl(f, buf, len, false);
}

bool data_writer_append_urgent(dw_file_t f, const char *buf, int len)
{
    return dw_append_impl(f, buf, len, true);
}

bool data_writer_append_stream(dw_file_t f, dw_emit_fn emit, void *arg)
{
    if (f >= DW_FILE_N || !emit || !s_ready) return false;
    bool ok = false;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        if (!s_stream.used) {
            s_stream.used = true;
            s_stream.f = f;
            s_stream.emit = emit;
            s_stream.arg = arg;
            ok = true;
        }
        xSemaphoreGive(s_lock);
    }
    return ok;
}

void data_writer_set_gate(bool allow) { s_gate_ok = allow; }
bool data_writer_gate_ok(void) { return s_gate_ok; }

esp_err_t data_writer_init(void)
{
    if (s_ready) return ESP_OK;

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    size_t stage = 0;
    for (int i = 0; i < DW_FILE_N; i++) {
        dw_entry_t *e = &s_files[i];
        if (!e->path || !e->buf_size) continue;
        if (e->buf_size > stage) stage = e->buf_size;
        e->buf = heap_caps_malloc(e->buf_size, MALLOC_CAP_SPIRAM);
        if (!e->buf) {
            ESP_LOGE(TAG, "%s 攒批缓冲分配失败 (%u B)", e->path, (unsigned)e->buf_size);
            return ESP_ERR_NO_MEM;
        }
        e->len = 0;
    }
    s_stage = heap_caps_malloc(stage, MALLOC_CAP_SPIRAM);
    if (!s_stage) {
        ESP_LOGE(TAG, "落盘暂存分配失败 (%u B)", (unsigned)stage);
        return ESP_ERR_NO_MEM;
    }
    s_wstat_ms = dw_now_ms();
    s_ready = true;
    /* 带上闸门/挂载状态 — 插上串口复位一次就能看出 /data 通不通 */
    ESP_LOGI(TAG, "就绪 (/data 统一写入, 缓冲 %u B PSRAM) — 闸门%s, /data %s",
             (unsigned)stage,
             s_gate_ok ? "开" : "关",
             sensor_logger_data_mounted() ? "已挂载" : "未挂载");
    /* 逐文件列出台账 — 哪个文件没注册上, 开机日志直接暴露 */
    for (int i = 0; i < DW_FILE_N; i++)
        if (s_files[i].path && s_files[i].buf)
            ESP_LOGI(TAG, "  %s 攒批 %us%s", s_files[i].path,
                     (unsigned)(s_files[i].flush_ms / 1000),
                     s_files[i].mode == DW_MODE_OVERWRITE ? " 覆盖写" : "");
    return ESP_OK;
}
