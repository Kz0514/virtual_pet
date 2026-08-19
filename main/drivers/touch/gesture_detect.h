/**
 * @file gesture_detect.h
 * @brief 手势识别接口
 */
#ifndef GESTURE_DETECT_H
#define GESTURE_DETECT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 可识别的手势事件 */
typedef enum {
    GESTURE_NONE = 0,
    GESTURE_WAKE_SCREEN,      /* 屏幕关闭时，双击或长按左侧键唤醒 */
    GESTURE_OPEN_MENU,        /* 屏幕开启且菜单关闭时，双击左侧键打开菜单 */
    GESTURE_CLOSE_MENU,       /* 菜单打开时，双击左侧键关闭菜单 */
    GESTURE_VOICE_TRIGGER,    /* 长按左侧键：手动触发语音对话 */
    GESTURE_MUTE_TOGGLE,      /* 三击左侧键切换静音 */
    GESTURE_SINGLE_TAP,       /* 菜单模式下单击确认（非菜单模式不产生; 菜单模式下零延迟发射） */
    GESTURE_PETTING_HEAD,     /* >=3 个顶部通道被触摸超过 500ms */
    GESTURE_SHAKE,            /* MPU6500 检测到摇晃 */
    GESTURE_SWIPE_LEFT,       /* 顶部滑块左滑 */
    GESTURE_SWIPE_RIGHT,      /* 顶部滑块右滑 */
    GESTURE_NAV_UP,           /* 右侧滑条轻点上半段(释放分类, 中央死区忽略) */
    GESTURE_NAV_DOWN,         /* 右侧滑条轻点下半段(释放分类, 中央死区忽略) */
    GESTURE_NAV_SLIDE_UP,     /* 右侧滑条上滑(位移>阈值, 一次触摸一个事件) */
    GESTURE_NAV_SLIDE_DOWN,   /* 右侧滑条下滑 */
    GESTURE_TOP_TAP_LEFT,     /* 顶部滑条轻点左半区(释放分类, 中央死区忽略) */
    GESTURE_TOP_TAP_RIGHT,    /* 顶部滑条轻点右半区(释放分类, 中央死区忽略) */
} gesture_event_t;

/** 处理触摸状态，识别手势事件（以约 50Hz 频率调用） */
void gesture_process(void);

/** 检查并消费一个手势事件（非阻塞） */
bool gesture_poll_event(gesture_event_t *out_event);

/** 手势事件回调（在 gesture_process 的调用上下文同步触发）。
 *  注册后事件不再进入 poll 队列; 传 NULL 恢复 poll 模式。 */
typedef void (*gesture_event_cb_t)(gesture_event_t ev);
void gesture_set_event_handler(gesture_event_cb_t cb);

/** 更新屏幕/菜单状态，供手势上下文使用 */
void gesture_set_screen_on(bool on);
void gesture_set_menu_active(bool active);
bool gesture_is_menu_active(void);
bool gesture_is_screen_on(void);

/** 重置击键/滑动序列状态(页面切换时调用, 防止跨页拼接双击/三击)。
 *  清空: 左键状态机、tap 计数、滑块导航/滑动子状态机、待发事件。 */
void gesture_reset_taps(void);

/** 右侧滑块：读取音量 0–100%（步长 5%），未触摸时返回 -1 */
int gesture_read_volume_pct(void);

/** 右侧滑块：读取亮度 0–100%（步长 5%），未触摸时返回 -1 */
int gesture_read_brightness_pct(void);

/** 顶部滑块：水平选择 -1（左）、0（无）、+1（右） */
int gesture_read_horizontal_select(void);

#ifdef __cplusplus
}
#endif

#endif /* GESTURE_DETECT_H */
