/** @file pat_detector.c @brief 摸头按住检测 — 顶部滑条中间三电极 (GPIO4/5/6)
 * 任一按住 → motou 动画保持循环; 手指离开 200ms → 结束回 idle.
 *
 * 与 GESTURE_PETTING_HEAD (≥3 顶部通道 500ms) 互补: 这里只需一根手指
 * 按在中间任一电极即可, 无时长门槛. LVGL 定时器 20ms 轮询, 保证
 * 200ms 释放判定精度 (触摸扫描本身 50Hz). */
#include "pat_detector.h"
#include "diary_mgr.h"
#include "touch_fpc.h"
#include "pet_avatar.h"
#include "pet_engine.h"
#include "tm6604.h"
#include "esp_log.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pat";

#define PAT_POLL_MS     20      /* 轮询周期 (触摸扫描 50Hz, 20ms 足够跟手) */
#define PAT_RELEASE_MS  200     /* 手指离开 200ms 后结束动画 */
#define PAT_ENGINE_COOLDOWN_MS 2000  /* 按住期间养成事件的重复触发冷却 */
#define PAT_VIBE_DUTY   52      /* 持续轻震占空比 (>50% 芯片才起振, 低于摇晃强度) */
#define PAT_VIBE_HOLD_MS 200    /* 每次刷新续震 200ms — 按住期间持续, 松手 ≤200ms 停止 */

static lv_timer_t *s_timer;
static bool        s_active;           /* motou 循环播放中 */
static bool        s_enabled = true;   /* 页面级开关 (设置页禁用摸头) */
static uint32_t    s_last_touch_tick;  /* 最近一次中间电极触摸时刻 */
static uint32_t    s_last_engine_tick; /* 最近一次 pet_engine 触发时刻 */

static void poll_cb(lv_timer_t *t)
{
    if (!s_enabled) return;
    uint32_t now = xTaskGetTickCount();

    if (touch_is_top_middle_pressed()) {
        s_last_touch_tick = now;
        if (!s_active) {
            s_active = true;
            ESP_LOGI(TAG, "按住中间电极 — motou 循环");
            pet_avatar_set_hold(true);
            pet_avatar_play_fast(PET_ANIM_PATHEAD);
            /* 养成联动: 摸头即时反馈, 按住期间每 2s 再触发一次 */
            pet_engine_trigger(PET_EVENT_TOUCH);
            diary_mgr_note_event(DIARY_EVENT_PETTING);  /* 日记互动上报 (3s 节流在内部) */
            s_last_engine_tick = now;
        } else if (pet_avatar_get_current() != PET_ANIM_PATHEAD) {
            /* 被 LLM 等其他来源换掉的动画 — 按住期间夺回控制权 */
            pet_avatar_play_fast(PET_ANIM_PATHEAD);
        }
        if (now - s_last_engine_tick >= pdMS_TO_TICKS(PAT_ENGINE_COOLDOWN_MS)) {
            s_last_engine_tick = now;
            pet_engine_trigger(PET_EVENT_TOUCH);
        }
        /* 持续轻震: 每次轮询续震 200ms (tm6604 停止定时器被重置),
         * 按住期间不间断, 松手后 ≤200ms 自然停止 */
        tm6604_vibrate(PAT_VIBE_DUTY, PAT_VIBE_HOLD_MS);
    } else if (s_active &&
               now - s_last_touch_tick >= pdMS_TO_TICKS(PAT_RELEASE_MS)) {
        /* 200ms 内无重新触摸 → 结束 (期间重触则无缝继续, 不重启) */
        ESP_LOGI(TAG, "手指离开 %dms — motou 结束", PAT_RELEASE_MS);
        s_active = false;
        pet_avatar_set_hold(false);
        pet_avatar_play_fast(PET_ANIM_IDLE);
    }
}

void pat_detector_init(void)
{
    s_active = false;
    s_last_touch_tick = 0;
    s_last_engine_tick = 0;
    s_timer = lv_timer_create(poll_cb, PAT_POLL_MS, NULL);
    lv_timer_set_repeat_count(s_timer, -1);
    ESP_LOGI(TAG, "摸头检测器就绪 (%dHz 轮询, 离开 %dms 结束)",
             1000 / PAT_POLL_MS, PAT_RELEASE_MS);
}

bool pat_detector_is_active(void)
{
    return s_active;
}

void pat_detector_set_enabled(bool en)
{
    if (s_enabled == en) return;
    s_enabled = en;
    if (!en && s_active) {
        /* 立即释放: 结束动画循环; 续震 ≤200ms 内自然停止 */
        ESP_LOGI(TAG, "禁用 — 释放摸头动画");
        s_active = false;
        pet_avatar_set_hold(false);
        pet_avatar_play_fast(PET_ANIM_IDLE);
    }
}
