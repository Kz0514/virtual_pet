/**
 * @file life_log.c
 * @brief 全量交互日志 — 记录每一次互动 (对话/摸头/摇晃/语音), 带时间戳
 *
 * 文件: /data/life/log.txt (FatFS 用户可见分区, USB 直读展示)。
 * 注意: LLM 上下文**不**从这里取 (仍只读 /cfg/memory.txt) —
 * 这份文件是给主人看的"生命记录"。
 *
 * 实现: 只是一层格式化 + 投递, 落盘全部交给 data_writer (DW_LIFE_LOG)。
 * 原实现自建 入队 + 4KB 栈专用任务, 现回收; 换来的是 TTS 播放/U盘模式/
 * 低电闸门/卷坏 四个条件由 data_writer 一处统一判断 (原先只判了 TTS),
 * 且滚动从 remove+rename 换成原地截断 —— remove 的 FAT 链释放更新一旦
 * 丢失就攒孤儿簇 (见 data_writer.c 的 dw_write_all 注释)。
 *
 * 语义变化: 时间戳取**调用时刻** (开口/被摸那一刻), 而非原先任务出队
 * (TTS 播完) 的时刻 —— 事件日志本就该记事件发生的时刻。
 * 代价: 掉电丢失窗口由"毫秒级"变成所在文件的攒批周期 (30s)。
 */
#include "life_log.h"
#include "data_writer.h"
#include "time_manager.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

static const char *TAG = "life_log";

#define LIFE_DIR "/data/life"
#define LIFE_LINE_MAX 256

void life_log_line(const char *fmt, ...)
{
    /* 行缓冲用 PSRAM 而非栈: 调用方是 LVGL 定时器 / WS 事件回调等小栈
     * 上下文 (内部 RAM 栈), 这里省 256B 比省一次 malloc 值 */
    char *buf = heap_caps_malloc(LIFE_LINE_MAX, MALLOC_CAP_SPIRAM);
    if (!buf) return;

    char ts[32];
    if (time_manager_is_synced()) {
        time_t t = (time_t)time_manager_get_unix_sec();
        struct tm tm;
        localtime_r(&t, &tm);
        snprintf(ts, sizeof(ts), "%02d-%02d %02d:%02d",
                 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    } else {
        snprintf(ts, sizeof(ts), "--:-- --:--"); /* 未校时 */
    }

    int n = snprintf(buf, LIFE_LINE_MAX, "[%s] ", ts);
    if (n < 0 || n >= LIFE_LINE_MAX) {
        free(buf);
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + n, LIFE_LINE_MAX - (size_t)n, fmt, ap);
    va_end(ap);
    if (m > 0) n += m;
    /* vsnprintf 返回的是"本该写多长", 截断时会 > 实际可容 — 必须夹一次,
     * 否则末尾补 '\n' 越界 */
    if (n > LIFE_LINE_MAX - 2) n = LIFE_LINE_MAX - 2;
    buf[n++] = '\n';

    data_writer_append(DW_LIFE_LOG, buf, n);
    free(buf);
}

esp_err_t life_log_init(void)
{
    /* 目录在落盘时也会幂等补建 (entry 的 dir 字段), 这里先建一次是为了
     * 首启/U盘模式下 /data/life 就已在位 — 免得主机看见一个空分区 */
    mkdir(LIFE_DIR, 0777);
    ESP_LOGI(TAG, "就绪 (%s/log.txt, 经 data_writer 攒批)", LIFE_DIR);
    return ESP_OK;
}
