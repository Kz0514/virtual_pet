/** @file home_interaction.c @brief 主页面交互集中组件
 *
 * 从 main.c 搬入: 摇动/敲击分发(轻重分流、震动闸门排空、养成触发、
 * 动画反馈、ws 上报)、物理交互短句、语音 10s 冷却 + 连续会话、
 * 摸头手势响应、静音占位。
 *
 * 页面切换时由 input_handler 调 home_interaction_set_enabled(false):
 * poll 只排空检测器事件不产生反应, 手势事件直接忽略。
 */
#include "home_interaction.h"
#include "shake_detector.h"
#include "tap_detector.h"
#include "tm6604.h"
#include "pet_engine.h"
#include "pet_avatar.h"
#include "ws_client.h"
#include "tts_client.h"
#include "chat_bubble.h"
#include "session_mgr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static const char *TAG = "home_ix";

static bool      s_enabled = true;   /* 页面级开关 (设置页禁用) */
static uint32_t  s_last_voice = 0;   /* 语音触发 10s 冷却 */

/* main.c 导出: 交互唤醒/亮度恢复 + 空闲计时重置 */
extern void main_screen_note_interaction(void);

void home_interaction_set_enabled(bool en)
{
    if (s_enabled == en) return;
    s_enabled = en;
    ESP_LOGI(TAG, "主页交互%s", en ? "启用" : "禁用");
}

/* 物理交互的本地短句反馈 — 仅在宠物空闲时播, 不压掉对话气泡 */
static const char *s_local_phrases[] = {
    "嘿嘿~", "好舒服呀", "再摸一下嘛", "嘻嘻",
};

static void maybe_local_phrase(void)
{
    if (tts_client_is_busy() ||
        pet_avatar_get_current() != PET_ANIM_IDLE) return;
    chat_bubble_show(
        s_local_phrases[esp_random() %
        (sizeof(s_local_phrases) / sizeof(s_local_phrases[0]))], 3000);
}

void home_interaction_poll(void)
{
    /* 排空闸门:
     *  - 马达震动期间 — 振动经外壳传给 DMP, 会被误判为摇动/敲击;
     *  - 非主页(设置页) — 只排空不处理, 回主页不会积压旧事件 */
    bool gated = !s_enabled || tm6604_is_vibrating();

    shake_event_t se;
    bool got_shake = shake_detector_poll(&se);
    tap_event_t te;
    bool got_tap = tap_detector_poll(&te);
    uint16_t dbg_mg;
    uint8_t  dbg_reason;
    bool got_dbg = tap_detector_poll_dbg(&dbg_mg, &dbg_reason);

    if (gated) return;   /* 事件已排空 */

    /* 摇动 — 轻摇/重摇按幅度分流; 可唤醒屏幕 */
    if (got_shake) {
        bool hard = se.magnitude_g >= 0.8f;
        main_screen_note_interaction();
        ESP_LOGI(TAG, "摇动! |a|=%.2fg (%s)", se.magnitude_g, hard ? "重" : "轻");
        tm6604_vibrate(70, hard ? 200 : 100);
        pet_engine_trigger(hard ? PET_EVENT_HARD_SHAKE : PET_EVENT_SHAKE);
        /* 本地即时反馈: 摇动 → 兴奋动画 */
        pet_avatar_play_fast(PET_ANIM_EXCITED);
        if (ws_client_is_connected()) {
            char evt[160];
            snprintf(evt, sizeof(evt),
                "{\"type\":\"sensor_event\",\"event\":\"shake\","
                "\"data\":{\"accel_mag\":%.2f,\"strength\":\"%s\"}}",
                se.magnitude_g, hard ? "hard" : "light");
            ws_client_send_json(evt);
        }
    }

    /* 敲击 — 单击=轻拍, 双击=加倍奖励; 可唤醒屏幕
     * 注意: 不振动反馈 — 马达振动会打到桌面被 DMP 误判为敲击, 形成自激环路 */
    if (got_tap) {
        main_screen_note_interaction();
        ESP_LOGI(TAG, "敲击 x%d (dir=%u) |a|=%.2fg", te.count, te.direction, te.magnitude_g);
        if (te.count >= 2) {
            pet_engine_trigger(PET_EVENT_DOUBLE_TOUCH);
        } else {
            pet_engine_trigger(PET_EVENT_TOUCH);
        }
        /* 本地即时反馈: 敲击不震动(防自激), 改播动画 + 空闲时短句 */
        pet_avatar_play_fast(te.count >= 2 ? PET_ANIM_EXCITED : PET_ANIM_BLUSH);
        maybe_local_phrase();
        if (ws_client_is_connected()) {
            char evt[128];
            snprintf(evt, sizeof(evt),
                "{\"type\":\"sensor_event\",\"event\":\"tap\","
                "\"data\":{\"count\":%u,\"direction\":%u}}",
                te.count, te.direction);
            ws_client_send_json(evt);
        }
    }

    /* 敲击诊断: 被拒的疑似敲击峰值 (mg) 与原因 */
    if (got_dbg) {
        ESP_LOGI(TAG, "敲击候选被拒: %umg (%s)", dbg_mg,
                 dbg_reason == 0 ? "低于阈值" : "静置不足");
    }
}

void home_interaction_on_gesture(gesture_event_t ev)
{
    if (!s_enabled) return;   /* input_handler 只在主页路由, 双保险 */

    switch (ev) {
    case GESTURE_PETTING_HEAD:
        main_screen_note_interaction();
        pet_engine_trigger(PET_EVENT_TOUCH);
        break;

    case GESTURE_VOICE_TRIGGER: {
        uint32_t now = xTaskGetTickCount();
        if (now - s_last_voice < pdMS_TO_TICKS(10000)) break;  /* 10s cooldown */
        s_last_voice = now;
        ESP_LOGI(TAG, "Voice trigger!");
        main_screen_note_interaction();
        pet_engine_trigger(PET_EVENT_VOICE);
        session_mgr_enter();   /* 进入连续会话; 会话中 = 打断进聆听 */
        break;
    }

    case GESTURE_MUTE_TOGGLE:
        /* 静音在后续阶段接入 (config_mgr) */
        chat_bubble_show("静音功能开发中…", 2000);
        break;

    default:
        break;
    }
}
