/**
 * @file time_manager.c
 * @brief NTP 时间同步 + 时区管理 — esp_netif_sntp (IDF 5.5.4)
 *
 * init 即开始轮询 (cfg.start=true), 无网络时 UDP 静默失败,
 * lwIP 按 CONFIG_LWIP_SNTP_UPDATE_DELAY (60s, sdkconfig.defaults) 自动重试,
 * 无需自写重试; 断线重连后下个轮询周期自动再同步, sync_cb 再次触发。
 *
 * 时区: 初始值按配置 (自动/手动) 应用; 自动模式下每日由
 * time_manager_daily_tz_tick 向服务端查 IP 定位时区 (Open-Meteo 换算),
 * 失败回退保持当前值。
 */
#include "time_manager.h"
#include "api_client.h"
#include "config_mgr.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

static const char *TAG = "time_manager";
static volatile bool   s_synced = false;
static volatile int32_t s_tz_offset = 28800;   /* 东八区缺省 */

#define CFG_KEY_TZ_AUTO    "tz_auto"       /* 1=自动 (IP 定位) 0=手动 */
#define CFG_KEY_TZ_MANUAL  "tz_manual_sec" /* 手动偏移秒 */
#define CFG_KEY_TZ_LAST    "tz_last"       /* 上次成功拉取 unix 秒 */

static void s_sync_cb(struct timeval *tv)
{
    s_synced = true;
    struct tm tm;
    localtime_r(&tv->tv_sec, &tm);   /* 已 tzset, 此处即本机时区 */
    ESP_LOGI(TAG, "NTP 同步成功: %04d-%02d-%02d %02d:%02d:%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

void time_manager_apply_tz(int32_t offset_sec)
{
    /* POSIX TZ 符号与偏移相反: 东八区 → "UTC-8" (无夏令时) */
    char tz[32];
    int32_t neg = -offset_sec;
    int h = (int)(neg / 3600);
    int m = (int)((neg % 3600) / 60);
    if (m == 0) {
        snprintf(tz, sizeof(tz), "UTC%+d", h);
    } else {
        snprintf(tz, sizeof(tz), "UTC%+d:%02d", h, (m < 0) ? -m : m);
    }
    setenv("TZ", tz, 1);
    tzset();
    s_tz_offset = offset_sec;
    ESP_LOGI(TAG, "时区已应用: %s (offset=%+ds)", tz, (int)offset_sec);
}

esp_err_t time_manager_init(void)
{
    /* 初始时区: 自动模式先用缺省 +8 等首次拉取; 手动模式直接用配置值 */
    uint32_t auto_tz = config_get_u32(CFG_KEY_TZ_AUTO, 1);
    uint32_t manual  = config_get_u32(CFG_KEY_TZ_MANUAL, 28800);
    time_manager_apply_tz(auto_tz ? 28800 : (int32_t)manual);
    if (auto_tz) ESP_LOGI(TAG, "自动时区模式 (首次拉取后更新)");
    else         ESP_LOGI(TAG, "手动时区模式");

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "ntp.tencent.com"));
    cfg.start = true;
    cfg.wait_for_sync = false;   /* 通知走 sync_cb */
    cfg.sync_cb = s_sync_cb;
    esp_err_t ret = esp_netif_sntp_init(&cfg);
    if (ret != ESP_OK)
        ESP_LOGE(TAG, "SNTP init 失败: %s (检查 sdkconfig LWIP_SNTP_MAX_SERVERS)",
                 esp_err_to_name(ret));
    return ret;
}

void time_manager_daily_tz_tick(void)
{
    if (config_get_u32(CFG_KEY_TZ_AUTO, 1) == 0) return;   /* 手动模式不动 */
    if (!api_client_is_authenticated()) return;            /* 未注册无法鉴权 */
    if (!time_manager_is_synced()) return;                 /* 时间未准, 无基准 */

    uint32_t now = time_manager_get_unix_sec();
    uint32_t last = config_get_u32(CFG_KEY_TZ_LAST, 0);
    if (now - last < 86400) return;                        /* 每日一次 */

    int32_t off = api_get_timezone_offset();
    if (off > 0) {
        config_set_u32(CFG_KEY_TZ_LAST, now);              /* 仅成功才记账 */
        time_manager_apply_tz(off);
    } else {
        ESP_LOGW(TAG, "时区拉取失败, 保持当前 (%+ds)", (int)s_tz_offset);
    }
}

bool time_manager_is_synced(void) { return s_synced; }

uint32_t time_manager_get_unix_sec(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)tv.tv_sec;
}

int32_t time_manager_get_tz_offset(void) { return s_tz_offset; }
