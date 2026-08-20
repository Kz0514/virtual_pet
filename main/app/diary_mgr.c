/**
 * @file diary_mgr.c
 * @brief 日记互动上报 + 今日计数
 *
 * 服务端日记按"当天互动次数"决定是否生成 (diary_min_interactions),
 * 互动事件中 shake/tap 由 home_interaction 直报, 摸头/语音在这里补报:
 *   联网: ws 发 {"type":"sensor_event","event":"petting"|"voice",...}
 *   离线: 只记本机计数, 不缓冲 (服务端传感器事件以在线为准, 可接受)
 * 计数: config_mgr 键 diary_day(YYYYMMDD) + diary_cnt; 跨天翻转清零;
 *       低电量 (memory_store_writes_safe()=false) 时跳过 NVS 写, 保持 RAM。
 */
#include "diary_mgr.h"
#include "config_mgr.h"
#include "time_manager.h"
#include "memory_store.h"
#include "ws_client.h"
#include "life_log.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <time.h>

static const char *TAG = "diary_mgr";

#define CFG_KEY_DAY  "diary_day"   /* 今日 YYYYMMDD */
#define CFG_KEY_CNT  "diary_cnt"   /* 今日互动计数 */
#define THROTTLE_MS  3000          /* 同事件最小间隔 (防 pat_detector 刷爆) */

static uint32_t s_day = 0;      /* 当前缓存的 YYYYMMDD, 0=未知 */
static uint32_t s_cnt = 0;      /* RAM 计数 (NVS 写受限时的可靠源) */
static uint32_t s_last[2];      /* 每事件上次上报 tick */

static const char *s_ev_name[] = {"petting", "voice"};

/* 今日 YYYYMMDD; 未校时返回 0 */
static uint32_t today_stamp(void)
{
    if (!time_manager_is_synced()) return 0;
    time_t t = (time_t)time_manager_get_unix_sec();
    struct tm tm;
    localtime_r(&t, &tm);
    return (uint32_t)(tm.tm_year + 1900) * 10000
         + (uint32_t)(tm.tm_mon + 1) * 100
         + (uint32_t)tm.tm_mday;
}

static void persist(void)
{
    /* 低电量不写 flash; RAM 计数保留, 恢复后由下次 note_event 补写 */
    if (!memory_store_writes_safe()) return;
    config_set_u32(CFG_KEY_DAY, s_day);
    config_set_u32(CFG_KEY_CNT, s_cnt);
}

esp_err_t diary_mgr_init(void)
{
    memset(s_last, 0, sizeof(s_last));
    s_day = config_get_u32(CFG_KEY_DAY, 0);
    s_cnt = config_get_u32(CFG_KEY_CNT, 0);
    uint32_t today = today_stamp();
    if (today && today != s_day) {
        ESP_LOGI(TAG, "跨天翻转: %u -> %u, 计数清零", s_day, today);
        s_day = today;
        s_cnt = 0;
        persist();
    } else {
        ESP_LOGI(TAG, "今日计数 %u (day=%u%s)", s_cnt, s_day,
                 today ? "" : ", 未校时");
    }
    return ESP_OK;
}

void diary_mgr_note_event(diary_event_t ev)
{
    if ((int)ev < 0 || (int)ev >= 2) return;

    uint32_t now = xTaskGetTickCount();
    if (now - s_last[ev] < pdMS_TO_TICKS(THROTTLE_MS)) return;
    s_last[ev] = now;

    /* 全量交互日志 (USB 直读) — 节流通过才记, 防刷屏 */
    life_log_line("%s", ev == DIARY_EVENT_PETTING ? "摸头" : "语音对话");

    /* 跨天翻转 (init 后过夜的情况) */
    uint32_t today = today_stamp();
    if (today && today != s_day) {
        s_day = today;
        s_cnt = 0;
    }

    /* 联网上报 — 与 shake/tap 同格式 */
    if (ws_client_is_connected()) {
        char evt[96];
        snprintf(evt, sizeof(evt),
                 "{\"type\":\"sensor_event\",\"event\":\"%s\","
                 "\"data\":{\"source\":\"gesture\"}}",
                 s_ev_name[ev]);
        if (ws_client_send_json(evt) == ESP_OK) {
            ESP_LOGI(TAG, "上报 %s", s_ev_name[ev]);
        }
    }

    /* 计数 — 未校时不知道"今天", 只上报不计数 (服务端已留痕) */
    if (today) {
        s_cnt++;
        persist();
    }
}

uint32_t diary_mgr_today_count(void)
{
    if (!time_manager_is_synced()) return 0;
    uint32_t today = today_stamp();
    if (today != s_day) return 0;
    return s_cnt;
}
