/**
 * @file gesture_detect.c
 * @brief 基于 FPC 触摸原始数据的手势识别
 *
 * 识别手势：单击、双击、三击、长按、滑动、抚摸。
 * 所有时序判断均在此处完成；touch_fpc 仅提供原始状态。
 */

#include "board.h"
#include "touch_fpc.h"
#include "gesture_detect.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

/* ── 手势检测状态机 ── */
typedef enum {
    GS_IDLE,            /* 无交互 */
    GS_PRESS_PENDING,   /* 检测到首次触摸，等待分类 */
    GS_SINGLE_TAP,      /* 单击已确认（等待可能的双击） */
    GS_DOUBLE_TAP,      /* 双击已确认（等待可能的三击） */
    GS_LONG_PRESS,      /* 长按进行中 */
    GS_SWIPING,         /* 滑动进行中 */
} gesture_state_t;

typedef struct {
    gesture_state_t state;
    uint32_t        press_start_us;     /* 当前按下开始的时间 */
    uint32_t        last_release_us;    /* 上次释放的时间 */
    uint32_t        last_tap_us;        /* 最近一次点击释放的时间 */
    int             tap_count;          /* 当前序列中的连续点击次数 */
    float           swipe_start_pos;    /* 滑动起始位置 */
    float           swipe_current_pos;  /* 滑动当前位置 */
    bool            screen_on;          /* 当前屏幕状态 */
    bool            menu_active;        /* UI 菜单是否打开 */
    gesture_event_t pending_event;      /* 待报告的事件 */
    bool            event_pending;
} gesture_ctx_t;

static gesture_ctx_t s_gctx = {
    .state        = GS_IDLE,
    .screen_on    = true,  /* 启动后屏幕默认开启 */
    .menu_active  = false,
    .event_pending = false,
};

#define TAP_TIMEOUT_US       300000   /* 点击间隔超时 300ms */
#define LONG_PRESS_US        3000000  /* 长按阈值 3s */
#define SWIPE_THRESHOLD      0.3f     /* 触发滑动的位移阈值 (raw 质心位移) */
#define DOUBLE_TAP_WINDOW_US 500000   /* 双击窗口 500ms */
#define TRIPLE_TAP_WINDOW_US 600000   /* 三击窗口 600ms */

/* 右侧滑条轻点 (NAV_UP/NAV_DOWN) */
#define NAV_TAP_MAX_MS       500      /* 按住超过此时长视为亮度调节等操作, 不发 NAV */
#define NAV_DEADZONE         0.08f    /* 中央死区: |pos| 小于此值忽略, 防误触 */
#define NAV_SETTLE_MS        100      /* 按下后 IIR 收敛等待, 之后采样作为滑动基准 */
#define NAV_SLIDE_THRESHOLD  0.25f    /* 右侧滑条滑动判定位移阈值 */

/* 右侧滑条跟踪 (轻点 NAV_UP/DOWN + 滑动 NAV_SLIDE_UP/DOWN) */
static struct {
    bool     tracking;       /* 按住中 */
    bool     settled;        /* IIR 已收敛, slide_ref 有效 */
    bool     slide_active;   /* 本次触摸已发过滑动事件 (释放时不再判轻点) */
    uint32_t press_us;       /* 按下时刻 */
    float    last_pos;       /* 按住期间最新位置 */
    float    slide_ref;      /* 滑动档位基准: 每越过一个阈值发一个事件 */
} s_nav = {0};

/* 顶部滑条跟踪 (滑动 SWIPE_LEFT/RIGHT + 轻点分区 TOP_TAP_LEFT/RIGHT) */
static struct {
    bool     tracking;       /* 触摸中 */
    bool     emitted;        /* 本次触摸已发射过滑动方向事件 */
    uint32_t press_us;       /* 按下时刻 */
    float    start_pos;      /* 按下时 raw 质心 */
    float    last_pos;       /* 最新 raw 质心 */
} s_swipe = {0};

static gesture_event_cb_t s_event_cb = NULL;

/* 事件出口: 注册了回调则同步调用(在 gesture_process 的运行上下文),
 * 否则进单槽队列由 gesture_poll_event 消费 */
static void emit_event(gesture_event_t ev)
{
    if (s_event_cb) {
        s_event_cb(ev);
        return;
    }
    s_gctx.pending_event = ev;
    s_gctx.event_pending = true;
}

/* ── 手势主处理（以约 50 Hz 频率调用） ── */
void gesture_process(void)
{
    bool left_pressed = touch_is_left_pressed();
    uint64_t now_us = esp_timer_get_time();

    switch (s_gctx.state) {

    case GS_IDLE:
        if (left_pressed) {
            s_gctx.state = GS_PRESS_PENDING;
            s_gctx.press_start_us = now_us;
            s_gctx.tap_count = 0;
        }
        break;

    case GS_PRESS_PENDING:
        if (!left_pressed) {
            /* 已释放 — 判断是否为点击 */
            uint32_t hold_ms = (uint32_t)((now_us - s_gctx.press_start_us) / 1000);

            if (hold_ms < 500) {
                if (s_gctx.menu_active) {
                    /* 菜单模式: 单击零延迟发射 — 双击已改为顶条右滑返回,
                     * 无竞争语义, 不必等 500ms 双击窗口 */
                    if (s_gctx.screen_on) {
                        emit_event(GESTURE_SINGLE_TAP);   /* 确认 */
                    } else {
                        emit_event(GESTURE_WAKE_SCREEN);  /* 息屏: 只唤醒不盲操作 */
                    }
                    s_gctx.state = GS_IDLE;
                    s_gctx.tap_count = 0;
                } else {
                    /* 主页模式: 短按 = 可能的点击, 累计等双击/三击 */
                    s_gctx.tap_count++;
                    s_gctx.last_tap_us = now_us;
                    s_gctx.state = GS_SINGLE_TAP;
                }
            } else {
                /* 长按后释放 — 无事件（长按已单独处理） */
                s_gctx.state = GS_IDLE;
                s_gctx.tap_count = 0;
            }
        } else if ((now_us - s_gctx.press_start_us) > LONG_PRESS_US) {
            /* 检测到长按 */
            s_gctx.state = GS_LONG_PRESS;
            if (!s_gctx.screen_on) {
                /* 长按唤醒屏幕 */
                emit_event(GESTURE_WAKE_SCREEN);
            } else {
                /* 长按触发语音对话 */
                emit_event(GESTURE_VOICE_TRIGGER);
            }
        }
        break;

    case GS_SINGLE_TAP:
        if (left_pressed) {
            s_gctx.state = GS_PRESS_PENDING;
            s_gctx.press_start_us = now_us;
        } else {
            /* 注意: 统一 µs 比较 — 曾误用 ms 值与 µs 窗口比较, 导致
             * 单击确认分支永假、任意两次按键都被算作双击 */
            uint32_t elapsed_us = (uint32_t)(now_us - s_gctx.last_tap_us);
            if (elapsed_us > DOUBLE_TAP_WINDOW_US) {
                /* 超时 — 确认为单击: 主页模式下单击无动作。
                 * (菜单模式的单击已在 GS_PRESS_PENDING 释放时零延迟发射) */
                s_gctx.state = GS_IDLE;
                s_gctx.tap_count = 0;
            }
            /* 否则：等待可能的第二次点击 */
        }
        break;

    case GS_DOUBLE_TAP:
        if (left_pressed) {
            s_gctx.state = GS_PRESS_PENDING;
            s_gctx.press_start_us = now_us;
        } else {
            uint32_t elapsed_us = (uint32_t)(now_us - s_gctx.last_tap_us);
            if (elapsed_us > TRIPLE_TAP_WINDOW_US) {
                /* 双击已确认 */
                if (!s_gctx.screen_on) {
                    emit_event(GESTURE_WAKE_SCREEN);
                } else if (!s_gctx.menu_active) {
                    emit_event(GESTURE_OPEN_MENU);
                } else {
                    emit_event(GESTURE_CLOSE_MENU);
                }
                s_gctx.state = GS_IDLE;
                s_gctx.tap_count = 0;
            }
        }
        break;

    case GS_LONG_PRESS:
        if (!left_pressed) {
            s_gctx.state = GS_IDLE;
        }
        break;

    case GS_SWIPING:
        /* 滑动手势通过滑块位置追踪处理 */
        if (!touch_is_left_pressed()) {
            s_gctx.state = GS_IDLE;
        }
        break;
    }

    /* 多次点击状态转换：单击 → 双击 → 三击 */
    if (s_gctx.state == GS_SINGLE_TAP && s_gctx.tap_count >= 2) {
        s_gctx.state = GS_DOUBLE_TAP;
    }
    if (s_gctx.state == GS_DOUBLE_TAP && s_gctx.tap_count >= 3) {
        /* 三击 = 快捷操作（静音切换） */
        emit_event(GESTURE_MUTE_TOGGLE);
        s_gctx.state = GS_IDLE;
        s_gctx.tap_count = 0;
    }

    /* 摸头事件: touch_fpc 内部判定 >=3 顶部通道持续 500ms; 2s 冷却防重复 */
    static uint32_t s_last_petting_us = 0;
    if (touch_is_petting_head() &&
        (uint32_t)(now_us - s_last_petting_us) > 2000000) {
        s_last_petting_us = now_us;
        emit_event(GESTURE_PETTING_HEAD);
    }

    /* ── 右侧滑条: 轻点 NAV_UP/DOWN + 滑动 NAV_SLIDE_UP/DOWN ──
     * 独立于左键状态机 (不同电极, 并行不冲突)。
     * filtered 是 IIR 缓升, 按下首帧质心失真 → 按下 100ms 后取基准点,
     * 按住期间持续采样最新位置。
     * 滑动 = 档位式: 相对基准每滑过一个阈值档位发一个事件 —
     * 短滑一格, 长滑连走多格 (回调模式下逐格同步分发, 列表侧钳位)。
     * 轻点: 短按释放且未触发滑动时按位置分区 (中央死区忽略)。
     * 长按无位移 (主页亮度调节等) 不发射。
     * 息屏时不发射 — 唤醒触摸不得被盲操作消费 (唤醒走 main 循环的
     * touched 检测)。
     * 方向标定 (2026-08-18 用户实测): 物理上半段 = pos 负侧。 */
    bool right_pressed = touch_is_right_pressed();
    if (right_pressed && !s_nav.tracking) {
        s_nav.tracking     = true;
        s_nav.settled      = false;
        s_nav.slide_active = false;
        s_nav.press_us     = (uint32_t)now_us;
        s_nav.last_pos     = touch_right_position();
    } else if (right_pressed) {
        s_nav.last_pos = touch_right_position();
        if (!s_nav.settled &&
            ((uint32_t)(now_us - s_nav.press_us) / 1000) >= NAV_SETTLE_MS) {
            s_nav.settled   = true;
            s_nav.slide_ref = s_nav.last_pos;
        }
        if (s_nav.settled && s_gctx.screen_on) {
            /* 一次触摸允许多个档位事件 (while 处理快速滑动的跨档) */
            while (s_nav.last_pos - s_nav.slide_ref < -NAV_SLIDE_THRESHOLD) {
                s_nav.slide_ref -= NAV_SLIDE_THRESHOLD;
                s_nav.slide_active = true;
                emit_event(GESTURE_NAV_SLIDE_UP);    /* 上滑 = pos 负方向 */
            }
            while (s_nav.last_pos - s_nav.slide_ref > NAV_SLIDE_THRESHOLD) {
                s_nav.slide_ref += NAV_SLIDE_THRESHOLD;
                s_nav.slide_active = true;
                emit_event(GESTURE_NAV_SLIDE_DOWN);
            }
        }
    } else if (s_nav.tracking) {
        s_nav.tracking = false;
        uint32_t hold_ms = (uint32_t)(now_us - s_nav.press_us) / 1000;
        if (hold_ms < NAV_TAP_MAX_MS && !s_nav.slide_active &&
            s_gctx.screen_on) {
            if (s_nav.last_pos < -NAV_DEADZONE) {
                emit_event(GESTURE_NAV_UP);      /* 物理上半段 = pos 负侧 */
            } else if (s_nav.last_pos > NAV_DEADZONE) {
                emit_event(GESTURE_NAV_DOWN);    /* 物理下半段 = pos 正侧 */
            }
        }
    }

    /* ── 顶部滑条: 滑动 SWIPE_LEFT/RIGHT + 轻点分区 TOP_TAP_LEFT/RIGHT ──
     * 独立子状态机 (与左键/摸头并行)。位置用 raw 质心 —
     * smooth 系数 0.3 会低估快速位移。滑动一次触摸只发一个方向事件。
     * 轻点分区: 短按释放且未触发滑动时按位置分类 (摸头是 >=500ms
     * 持续触摸, 与 <500ms 轻点不冲突); 是否路由由仲裁器按页面/模式决定。
     * 息屏时不发射 — 设置页右滑=返回, 盲操作会意外退出设置页。 */
    bool top_pressed = touch_is_top_pressed();
    if (top_pressed && !s_swipe.tracking) {
        s_swipe.tracking  = true;
        s_swipe.emitted   = false;
        s_swipe.press_us  = (uint32_t)now_us;
        s_swipe.start_pos = touch_top_position_raw();
        s_swipe.last_pos  = s_swipe.start_pos;
    } else if (top_pressed) {
        s_swipe.last_pos = touch_top_position_raw();
        if (!s_swipe.emitted) {
            float pos = s_swipe.last_pos;
            if (s_gctx.screen_on && pos - s_swipe.start_pos > SWIPE_THRESHOLD) {
                s_swipe.emitted = true;
                emit_event(GESTURE_SWIPE_RIGHT);
            } else if (s_gctx.screen_on &&
                       s_swipe.start_pos - pos > SWIPE_THRESHOLD) {
                s_swipe.emitted = true;
                emit_event(GESTURE_SWIPE_LEFT);
            }
        }
    } else if (s_swipe.tracking) {
        s_swipe.tracking = false;
        uint32_t hold_ms = (uint32_t)(now_us - s_swipe.press_us) / 1000;
        if (hold_ms < NAV_TAP_MAX_MS && !s_swipe.emitted &&
            s_gctx.screen_on) {
            if (s_swipe.last_pos < -NAV_DEADZONE) {
                emit_event(GESTURE_TOP_TAP_LEFT);    /* 左半区 */
            } else if (s_swipe.last_pos > NAV_DEADZONE) {
                emit_event(GESTURE_TOP_TAP_RIGHT);   /* 右半区 */
            }
        }
    }
}

/* ── 轮询手势事件 ── */
bool gesture_poll_event(gesture_event_t *out_event)
{
    if (s_event_cb) return false;  /* 回调模式: 事件不再进队列 */
    if (s_gctx.event_pending) {
        *out_event = s_gctx.pending_event;
        s_gctx.event_pending = false;
        s_gctx.pending_event = GESTURE_NONE;
        return true;
    }
    return false;
}

void gesture_set_event_handler(gesture_event_cb_t cb)
{
    s_event_cb = cb;
    if (cb) {
        /* 切到回调模式时清掉遗留的队列事件 */
        s_gctx.event_pending = false;
        s_gctx.pending_event = GESTURE_NONE;
    }
}

/* ── 状态设置器（由屏幕/菜单管理器调用） ── */
void gesture_set_screen_on(bool on)  { s_gctx.screen_on = on; }
void gesture_set_menu_active(bool a) { s_gctx.menu_active = a; }
bool gesture_is_menu_active(void)    { return s_gctx.menu_active; }
bool gesture_is_screen_on(void)      { return s_gctx.screen_on; }

void gesture_reset_taps(void)
{
    s_gctx.state         = GS_IDLE;
    s_gctx.tap_count     = 0;
    s_gctx.press_start_us = 0;
    s_gctx.last_tap_us   = 0;
    s_gctx.last_release_us = 0;
    s_gctx.event_pending = false;
    s_gctx.pending_event = GESTURE_NONE;
    s_nav.tracking     = false;
    s_nav.settled      = false;
    s_nav.slide_active = false;
    s_swipe.tracking = false;
    s_swipe.emitted  = false;
}

/* ── 基于滑块的数值调节 ── */
int gesture_read_volume_pct(void)
{
    /* 右侧滑块上半部分 = 音量 */
    float pos = touch_right_position();
    if (fabsf(pos) < 0.15f) return -1;  /* 未被触摸 */
    /* 映射 0..1 到 0-100，步长 5% */
    int pct = (int)((pos + 1.0f) / 2.0f * 100.0f);
    pct = (pct / 5) * 5;  /* 量化为 5% 步长 */
    if (pct > 100) pct = 100;
    if (pct < 0)   pct = 0;
    return pct;
}

int gesture_read_brightness_pct(void)
{
    /* 右侧滑块 = 亮度, 阈值 0.15 过滤噪声 */
    float pos = touch_right_position();
    if (fabsf(pos) < 0.15f) return -1;
    int pct = (int)((pos + 1.0f) / 2.0f * 100.0f);
    pct = (pct / 5) * 5;
    if (pct > 100) pct = 100;
    if (pct < 0)   pct = 0;
    return pct;
}

int gesture_read_horizontal_select(void)
{
    /* 顶部滑块用于水平选择，返回 -1、0 或 +1 */
    float pos = touch_top_position();
    if (fabsf(pos) < 0.2f) return 0;
    return (pos > 0) ? 1 : -1;
}
