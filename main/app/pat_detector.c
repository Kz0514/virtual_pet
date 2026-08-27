/** @file pat_detector.c @brief 摸头滑动检测 — 顶部滑条 (5 通道) 上来回滑动
 * 触发 motou 动画保持循环 + 持续轻震; 无有效活动 200ms 结束回 idle.
 *
 * 触发语义 (2026-08-27 用户拍板): 惰性"撸猫" — 滑动质心
 * (touch_top_position_raw, 未平滑, 快速位移不低估) 方向翻转且单程行程
 * ≥ PAT_MIN_STROKE 计一次"撸", 首次撸进入摸头。手指停住不动/离手均
 * 无活动 → 200ms 后结束 (按在滑条上不再触发, 区别于旧"按住"语义)。
 * 震动与换代节奏与旧按住行为同步: 每次有效位移续震 200ms (连续滑动
 * 期持续轻震), 无活动自然停止; 养成事件 2s 冷却触发。
 */
#include "pat_detector.h"
#include "diary_mgr.h"
#include "touch_fpc.h"
#include "pet_avatar.h"
#include "pet_engine.h"
#include "tm6604.h"
#include "gesture_detect.h"
#include "power_manager.h"
#include "esp_log.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

static const char *TAG = "pat";

#define PAT_POLL_MS 20              /* 轮询周期 (触摸扫描 50Hz, 20ms 足够跟手) */
#define PAT_RELEASE_MS 600          /* 无有效活动 (停住/离手) 600ms 后结束 — v2: 200→600 停顿容忍 */
#define PAT_ENGINE_COOLDOWN_MS 2000 /* 摸头期间养成事件重复触发冷却 */
#define PAT_VIBE_DUTY 52            /* 持续轻震占空比 (>50% 芯片才起振, 低于摇晃强度) */
#define PAT_VIBE_HOLD_MS 200        /* 每次活动续震 200ms — 连续滑动期持续, 停手 ≤200ms 停止 */

/* 往返滑动判定 */
#define PAT_DEADZONE 0.05f   /* 质心位移死区 — 静止抖动不产生活动 */
#define PAT_MIN_STROKE 0.20f /* 单程行程门限 (质心 ±1.0 满幅) — 真机标定 v2: 0.35→0.20 */
#define PAT_TRAVEL_MAX 1.2f  /* 单程累计钳位 (防单方向长滑虚增行程) */

static lv_timer_t *s_timer;
static bool s_active;               /* motou 循环播放中 */
static bool s_enabled = true;       /* 页面级开关 (设置页禁用摸头) */
static pat_state_cb_t s_cb = NULL;
static void *s_cb_ctx = NULL;

/* 滑动轨迹 (raw 质心 -1..+1) */
static bool  s_has_pos;   /* 本次触点会话已有基准点 */
static float s_pos;       /* 上次质心 */
static int   s_dir;       /* 上次运动方向 (+1/-1, 0=未知) */
static float s_travel;    /* 当前方向累计行程 */

static uint32_t s_last_motion;      /* 最近一次有效位移时刻 (无活动超时判据) */
static uint32_t s_last_engine_tick; /* 最近一次 pet_engine 触发时刻 */

static void pat_begin(uint32_t now)
{
    s_active = true;
    ESP_LOGI(TAG, "滑动摸头 — motou 循环");
    pet_avatar_set_hold(true);
    pet_avatar_play_fast(PET_ANIM_PATHEAD);
    /* 养成联动: 摸头即时反馈, 滑动期间每 2s 再触发一次 */
    pet_engine_trigger(PET_EVENT_TOUCH);
    diary_mgr_note_event(DIARY_EVENT_PETTING); /* 日记互动上报 (3s 节流在内部) */
    s_last_engine_tick = now;
    if (s_cb)
        s_cb(true, s_cb_ctx);
}

static void pat_end(const char *why)
{
    s_active = false;
    ESP_LOGI(TAG, "%s — motou 结束", why);
    pet_avatar_set_hold(false);
    pet_avatar_play_fast(PET_ANIM_IDLE);
    s_has_pos = false;
    s_dir = 0;
    s_travel = 0;
    if (s_cb)
        s_cb(false, s_cb_ctx);
}

static void poll_cb(lv_timer_t *t)
{
    if (!s_enabled) return;
    uint32_t now = xTaskGetTickCount();

    if (touch_is_top_pressed()) {
        /* 息屏滑动 → 先唤醒: 顶部电极的触摸不进 gesture 左键唤醒路径
         * (单击/双击/长按只覆盖导航电极), 主循环轮询 >250 才有接触判定 —
         * 轻按 (<250) 会无反应。唤醒幂等
         * (power_manager_note_interaction 内部有息屏守卫)。 */
        if (!gesture_is_screen_on()) {
            power_manager_note_interaction();
        }
        float p = touch_top_position_raw();
        if (s_has_pos) {
            float d = p - s_pos;
            if (fabsf(d) >= PAT_DEADZONE) {
                s_last_motion = now; /* 有效位移 = 活动 */
                int dir = (d > 0) ? 1 : -1;
                if (s_dir != 0 && dir != s_dir) {
                    /* 方向翻转: 上段单程行程达标 → 一次"撸" */
                    if (s_travel >= PAT_MIN_STROKE) {
                        if (!s_active)
                            pat_begin(now);
                        /* 轻震续 200ms — 连续撸持续震, 无活动 ≤200ms 自然停止 */
                        tm6604_vibrate(PAT_VIBE_DUTY, PAT_VIBE_HOLD_MS);
                        if (now - s_last_engine_tick >= pdMS_TO_TICKS(PAT_ENGINE_COOLDOWN_MS)) {
                            s_last_engine_tick = now;
                            pet_engine_trigger(PET_EVENT_TOUCH);
                        }
                    }
                    s_travel = 0;
                    s_dir = dir;
                } else {
                    if (s_active)
                        tm6604_vibrate(PAT_VIBE_DUTY, PAT_VIBE_HOLD_MS); /* 活动期持续轻震 */
                    s_dir = dir;
                    s_travel += fabsf(d);
                    if (s_travel > PAT_TRAVEL_MAX)
                        s_travel = PAT_TRAVEL_MAX;
                }
            }
        } else {
            s_has_pos = true; /* 触点会话建立, 首帧只采样不做位移判定 */
        }
        s_pos = p;
    } else {
        /* 离手: 清轨迹 — 下次触点会话重新建立基准 */
        s_has_pos = false;
        s_dir = 0;
        s_travel = 0;
    }

    /* 无活动超时 (停住不动或离手) → 结束 */
    if (s_active && now - s_last_motion >= pdMS_TO_TICKS(PAT_RELEASE_MS))
        pat_end("无活动 200ms");
}

void pat_detector_init(void)
{
    s_active = false;
    s_last_motion = 0;
    s_last_engine_tick = 0;
    s_has_pos = false;
    s_dir = 0;
    s_travel = 0;
    s_timer = lv_timer_create(poll_cb, PAT_POLL_MS, NULL);
    lv_timer_set_repeat_count(s_timer, -1);
    ESP_LOGI(TAG, "摸头检测器就绪 (%dHz 轮询, 无活动 %dms 结束)",
             1000 / PAT_POLL_MS, PAT_RELEASE_MS);
}

void pat_detector_set_cb(pat_state_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;
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
        pat_end("禁用释放");
    }
}