/**
 * @file power_diag.c
 * @brief 功耗/睡眠离线诊断 (⑤-1 从 main.c 原样搬移, 零逻辑改动)
 *
 * power_log.csv 2s 一条 + power_seg.csv 60s 段统计/W 行唤醒记账
 * + 息屏锁/timer dump + tasks.txt — 拔电期间串口死, 只能靠这些 CSV/文件
 * 回看功耗与唤醒。fd 路径零分配, 禁 fopen (SRAM 紧张 abort); 唯一例外:
 * 息屏锁 dump (esp_pm_dump_locks/esp_timer_dump 官方 API 硬依赖 FILE*,
 * 内部堆 <8KB 时跳过)。
 */
#include "power_diag.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "esp_heap_caps.h"
#include "driver/usb_serial_jtag.h"
#include "power_manager.h"
#include "time_manager.h"
#include "touch_fpc.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static const char *TAG = "pwr_diag";

/* : WiFi TSF 激活查询 — 声明于 esp_private/wifi.h:524 (预编译库实现)。
 * 息屏后直查: 1 = WiFi skip 回调在阻断轻睡 (见 wifi_manager_detach_light_sleep_skip) */
bool esp_wifi_internal_is_tsf_active(void);

/* : FreeRTOS-Kernel tasks.c 诊断探针 (本地 patch, prvGetExpectedIdleTime
 * 每次评估时更新) — 定位轻睡窗口来源。dexp/ddel 受采样偏差污染 (采样时
 * main 在 ready 列表/未阻塞), 真实窗口来源 = winname/winrem (窗口评估瞬间
 * 延迟列表头部任务名 + 剩余 ticks, 不受采样偏差影响) */
extern volatile uint32_t xDiagExpectedIdle;
extern volatile TickType_t xDiagNextUnblock;
extern volatile TickType_t xDiagTickCount;
extern volatile uint32_t xDiagEvalCnt;
extern volatile uint32_t xDiagRet0Cnt;  /* : 评估 xReturn=0 (有就绪任务, 不睡) 累计 */
extern volatile uint32_t xDiagRet1Cnt;  /* : 评估 xReturn=1 (头部 1 tick 后到期) 累计 */
extern volatile uint32_t xDiagWinCnt;
extern volatile uint32_t xDiagWinCore;
extern volatile TickType_t xDiagWinRemain;
extern volatile char xDiagWinName[16];
extern volatile uint32_t xDiagSleepErr;    /* : esp_light_sleep_start 最后错误码 */
extern volatile uint32_t xDiagSleepErrCnt; /* : 错误累计计数 */

/* /data/power_log.csv — 2s 一条追加 (state: 0=亮 1=变暗 2=息屏), >64KB 重开。
 * fd 路径零分配 — newlib fopen 分配 FILE+锁 (内部 RAM), 耗尽直接 abort。
 * 超限重开: 不 remove (FAT 释放链更新丢失会累积孤儿簇 → U盘卷可用空间
 * 持续缩水; chkdsk 曾一次找回 37 条孤儿链 320KB), 改 O_TRUNC 原地截断
 * 复用簇链 — 簇始终被文件引用, FAT 更新丢失最坏只是长度回退, 不产生
 * 不可达簇。无状态实现: 每次写入前查文件当前大小, 超限即截断 — 重启
 * 丢失任何内存标志也不影响 (教训: 内存标志版被重启打断后 550KB 不再截断) */
static void power_diag_roll(const char *path, int64_t limit)
{
    int fd = open(path, O_CREAT | O_APPEND | O_WRONLY);
    if (fd < 0)
        return;
    if (lseek(fd, 0, SEEK_END) > limit) {
        close(fd);
        int t = open(path, O_TRUNC | O_WRONLY);
        if (t >= 0)
            close(t);
    } else {
        close(fd);
    }
}

void power_diag_log_append(const bq27220_data_t *bat, int state)
{
    power_diag_roll("/data/power_log.csv", 64 * 1024);
    int fd = open("/data/power_log.csv", O_CREAT | O_APPEND | O_WRONLY);
    if (fd < 0)
        return;
    if (lseek(fd, 0, SEEK_END) == 0) {
        static const char hdr[] = "ms,mv,ma,soc,state,sleeps,rejects,winus,nxtalm,dexp,ddel,sof,tsf,prob,evalcnt,ret0,ret1,wincnt,wincore,winrem,winname,err,errcnt,wk,ph,tc,d0,d1,d2,d3,d4,d5,d6,d7,d8,d9,d10,d11\n";
        write(fd, hdr, sizeof(hdr) - 1);
    }
    /* 离线诊断列: 电源/睡眠/触摸三类 — sleeps=轻睡评估次数 (enter_cb),
     * rejects=评估未真睡, winus=最后睡眠窗口 µs, sof/tsf/prob 见下,
     * nxtalm=esp_timer 最早可唤醒 alarm 距现在 µs (巨大=窗口来自 tick 列表),
     * dexp/ddel/evalcnt/ret0/ret1/wincnt/wincore/winrem/winname = tasks.c
     * 内核 patch 探针 (xDiag*), 定位窗口来源; err/errcnt=esp_light_sleep_start
     * 错误码/计数。拔电期间串口死, 只能靠 CSV 判轻睡 */
    uint32_t slp = 0, over30 = 0, rej = 0;
    int64_t wmax = 0, wlast = 0;
    power_manager_get_sleep_stats(&slp, &over30, &wmax, &wlast);
    rej = power_manager_get_rejects();
    bool sof = usb_serial_jtag_is_connected();
    bool tsf = esp_wifi_internal_is_tsf_active();
    uint32_t prob = power_manager_get_sleep_probe();
    int64_t nxtalm = esp_timer_get_next_alarm_for_wake_up() - esp_timer_get_time();
    uint32_t dexp = (uint32_t)xDiagExpectedIdle;
    int64_t ddel = (int64_t)(xDiagNextUnblock - xDiagTickCount);
    /* 触摸诊断列: ph=探针去抖命中数, tc=超阈值通道数, d0..d11=
     * 每通道 delta (raw−baseline, 有符号) — 供睡眠供电偏移形态分析 */
    uint32_t tr[12], tb[12];
    touch_get_raw(tr);
    touch_get_baseline(tb);
    int32_t td[12];
    for (int i = 0; i < 12; i++)
        td[i] = (int32_t)tr[i] - (int32_t)tb[i];
    char line[512];
    int ln = snprintf(line, sizeof(line),
                      "%lld,%u,%d,%u,%d,%u,%u,%lld,%lld,%u,%lld,%d,%d,%u,%u,%u,%u,%u,%u,%u,%s,%u,%u,%u,%u,%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                      (long long)(time_manager_is_synced() ? time_manager_get_unix_sec() * 1000LL : 0),
                      bat->voltage_mv, bat->current_ma, bat->soc_pct, state,
                      (unsigned)slp, (unsigned)rej, (long long)wlast,
                      (long long)nxtalm, (unsigned)dexp, (long long)ddel,
                      (int)sof, (int)tsf, (unsigned)prob,
                      (unsigned)xDiagEvalCnt, (unsigned)xDiagRet0Cnt,
                      (unsigned)xDiagRet1Cnt, (unsigned)xDiagWinCnt,
                      (unsigned)xDiagWinCore, (unsigned)xDiagWinRemain,
                      xDiagWinName,
                      (unsigned)xDiagSleepErr, (unsigned)xDiagSleepErrCnt,
                      (unsigned)power_manager_get_wake_cause(),
                      (unsigned)touch_fpc_probe_hits(), (unsigned)touch_fpc_touched_count(),
                      (int)td[0], (int)td[1], (int)td[2], (int)td[3], (int)td[4], (int)td[5],
                      (int)td[6], (int)td[7], (int)td[8], (int)td[9], (int)td[10], (int)td[11]);
    if (ln > 0)
        write(fd, line, (size_t)ln);
    off_t sz = lseek(fd, 0, SEEK_END);
    close(fd);
    (void)sz; /* 重开由 power_diag_roll 在写入前无状态检查完成 */
}

/* ── 分段功耗/唤醒统计: /data/power_seg.csv (256KB 环形 ≈25h 全量保留,
 * 补 power_log.csv 64KB 只留 ~10min 的验收盲区)。两型行:
 *   S 行: 每 60s 聚合 = 段内平均电流/电压/SOC + 亮屏时长 + 唤醒次数
 *   W 行: 每次亮屏翻转瞬间 = 时刻 (unix+uptime) + 来源 + 当时电量
 * 来源 src: 1=探针 2 连击 (假唤醒嫌疑), 0=其他 (左键/摇动/操作)。
 * fd 零分配路径遍历, 禁 fopen (SRAM 紧张 abort) */
#define SEG_MS (60 * 1000 * 1000LL) /* esp_timer 段长 60s */

static int64_t s_seg_start_us = 0;   /* 当前段起点 */
static int64_t s_seg_last_us = 0;    /* 上次 2s tick 时刻 (亮屏时长差) */
static int64_t s_seg_on_us = 0;      /* 段内亮屏累计 µs */
static int32_t s_seg_ma_sum = 0;     /* 段内电流累计 */
static uint32_t s_seg_ma_cnt = 0, s_seg_mv_cnt = 0, s_seg_soc_sum = 0;
static uint32_t s_seg_mv_sum = 0;
static uint32_t s_seg_wake_cnt = 0;  /* 段内唤醒次数 */
static bool s_bat_last_ok = false;   /* W 行电量取最近一次 bq27220 读数 */
static uint16_t s_bat_last_mv = 0;
static int16_t s_bat_last_ma = 0;
static uint8_t s_bat_last_soc = 0;

static void power_diag_seg_write_row(const char *buf, int len)
{
    power_diag_roll("/data/power_seg.csv", 256 * 1024);
    int fd = open("/data/power_seg.csv", O_CREAT | O_APPEND | O_WRONLY);
    if (fd < 0)
        return;
    if (lseek(fd, 0, SEEK_END) == 0) {
        static const char hdr[] = "type,ts_ms,up_ms,ma,mv,soc,src,wake_cnt,on_ms,seg_ms\n";
        write(fd, hdr, sizeof(hdr) - 1);
    }
    if (len > 0)
        write(fd, buf, (size_t)len);
    off_t sz = lseek(fd, 0, SEEK_END);
    close(fd);
    (void)sz; /* 重开由 power_diag_roll 在写入前无状态检查完成 */
}

/* 唤醒翻转点调用: 记录 W 行 (主要靠二次分析来源)。
 * ③-4′ 桥已收敛: power_manager 唤醒全流程经 note_interaction 调本函数记账 */
void power_diag_note_wake_source(uint8_t wake_src)
{
    s_seg_wake_cnt++;
    char line[160];
    int ln = snprintf(line, sizeof(line),
                      "W,%lld,%u,%d,%d,%u,%u,1,0,0\n",
                      (long long)(time_manager_is_synced() ? time_manager_get_unix_sec() * 1000LL : 0),
                      (unsigned)xTaskGetTickCount(),
                      (int)(s_bat_last_ok ? s_bat_last_ma : 0),
                      (int)(s_bat_last_ok ? s_bat_last_mv : 0),
                      (unsigned)(s_bat_last_ok ? s_bat_last_soc : 0),
                      (unsigned)wake_src);
    power_diag_seg_write_row(line, ln);
}

/* 2s 块调用 (have_bat 时): 累积段统计, 每 60s 落 S 行 */
void power_diag_seg_tick(const bq27220_data_t *bat)
{
    int64_t nowus = esp_timer_get_time();
    s_bat_last_ok = true;
    s_bat_last_mv = (uint16_t)bat->voltage_mv;
    s_bat_last_ma = (int16_t)bat->current_ma;
    s_bat_last_soc = (uint8_t)bat->soc_pct;
    if (s_seg_last_us && s_seg_start_us) {
        if (power_manager_is_screen_on())
            s_seg_on_us += (nowus - s_seg_last_us);
        s_seg_ma_sum += bat->current_ma;
        s_seg_ma_cnt++;
        s_seg_mv_sum += bat->voltage_mv;
        s_seg_mv_cnt++;
        s_seg_soc_sum += bat->soc_pct;
    } else {
        s_seg_start_us = nowus;
        s_seg_ma_sum = s_seg_mv_sum = 0;
        s_seg_ma_cnt = s_seg_mv_cnt = s_seg_soc_sum = 0;
        s_seg_on_us = 0;
        s_seg_wake_cnt = 0;
    }
    s_seg_last_us = nowus;
    if (nowus - s_seg_start_us >= SEG_MS) {
        char line[160];
        int ln = snprintf(line, sizeof(line),
                          "S,%lld,%u,%d,%d,%u,0,%u,%u,%d\n",
                          (long long)(time_manager_is_synced() ? time_manager_get_unix_sec() * 1000LL : 0),
                          (unsigned)xTaskGetTickCount(),
                          s_seg_ma_cnt ? (int)(s_seg_ma_sum / (int32_t)s_seg_ma_cnt) : 0,
                          s_seg_mv_cnt ? (int)(s_seg_mv_sum / s_seg_mv_cnt) : 0,
                          (unsigned)(s_seg_mv_cnt ? (s_seg_soc_sum + s_seg_mv_cnt / 2u) / s_seg_mv_cnt : 0),
                          (unsigned)(s_seg_on_us / 1000), (unsigned)s_seg_wake_cnt,
                          (int)((nowus - s_seg_start_us) / 1000));
        power_diag_seg_write_row(line, ln);
        s_seg_ma_sum = s_seg_mv_sum = 0;
        s_seg_ma_cnt = s_seg_mv_cnt = s_seg_soc_sum = 0;
        s_seg_on_us = 0;
        s_seg_wake_cnt = 0;
        s_seg_start_us = nowus;
    }
}

/* 探针: 轻睡窗口 (esp_timer 最早非SKIP alarm 距现在) +
 * vApplicationSleep 回调内统计 (30s 一次, 减日志量)。2s 节拍内调用,
 * 与屏幕状态无关 */
void power_diag_pm_stats_log(void)
{
    static uint8_t pm_log_cnt = 0;
    bool pm_log_now = (++pm_log_cnt >= 15);
    if (pm_log_now) pm_log_cnt = 0;
    int64_t pm_now = esp_timer_get_time();
    int64_t pm_next = esp_timer_get_next_alarm_for_wake_up();
    if (pm_log_now)
        ESP_LOGI(TAG, "PM窗: %lld us", (long long)(pm_next - pm_now));

    uint32_t w_total, w_over30;
    int64_t w_max, w_last;
    power_manager_get_sleep_stats(&w_total, &w_over30, &w_max, &w_last);
    ESP_LOGI(TAG, "PM统: cb=%u 超30ms=%u max=%lld last=%lld err=%u(0x%x,%u)",
             (unsigned)w_total, (unsigned)w_over30,
             (long long)w_max, (long long)w_last,
             (unsigned)xDiagSleepErr, (unsigned)xDiagSleepErr,
             (unsigned)xDiagSleepErrCnt);
}

/* 息屏每 60s 诊断一次: 轻睡计数 + PM 锁列表 */
void power_diag_screen_off_diag(void)
{
    static uint8_t diag_cnt = 0;
    if (++diag_cnt >= 7) { /* 息屏 15s 一次诊断 (加密锁采样) */
        diag_cnt = 0;
        power_manager_dump_stats();
        /* 锁/计时器 dump 写进 power_log.csv (# 注释行) —
         * 拔电期间串口死, 只能靠这里看。fopen 堆守卫:
         * newlib fopen 分配 FILE+锁, 内部堆耗尽 abort;
         * 不足 8KB 跳过 (锁列表仍走上面日志) */
        if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 8192) {
            ESP_LOGW(TAG, "内部堆不足, 跳过 power_log.csv 锁诊断");
        } else {
            FILE *lf = fopen("/data/power_log.csv", "a");
            if (lf) {
                fseek(lf, 0, SEEK_END);
                fprintf(lf, "# locks @ %lld\n",
                        (long long)(time_manager_is_synced() ? time_manager_get_unix_sec() * 1000LL : 0));
                esp_pm_dump_locks(lf);
                /* esp_timer dump — 定位周期 alarm 来源
                 * (任务名直接可见) */
                fprintf(lf, "# timers @ %lld\n",
                        (long long)(time_manager_is_synced() ? time_manager_get_unix_sec() * 1000LL : 0));
                esp_timer_dump(lf);
                fclose(lf);
            }
        } /* else: 内部堆充足才写 CSV 诊断 */
        /* 任务延迟探针 — vTaskList 列每任务状态 + 剩余 delay tick:
         * tick 列表高频到期任务会阻轻睡 (prvGetExpectedIdle
         * Time < 3 → vApplicationSleep 永不调用)。写
         * /data/tasks.txt 覆盖, U盘拷出 */
        static char s_tasklist[2048];
        vTaskList(s_tasklist);
        int tfd = open("/data/tasks.txt", O_CREAT | O_TRUNC | O_WRONLY);
        if (tfd >= 0) {
            write(tfd, s_tasklist, strlen(s_tasklist));
            close(tfd);
        }
    }
}
