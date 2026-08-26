/**
 * @file touch_fpc.h
 * @brief FPC 触摸传感器接口
 */
#ifndef TOUCH_FPC_H
#define TOUCH_FPC_H

#include "esp_err.h"
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 12 通道电容触摸传感器 */
esp_err_t touch_fpc_init(void);

/** 扫描所有通道并更新状态（周期性调用或由 LVGL 输入设备调用） */
void touch_fpc_scan(void);

/** LVGL 输入设备读取回调 */
bool touch_fpc_read(lv_indev_t *indev, lv_indev_data_t *data);

/** 左侧功能键是否按下 */
bool touch_is_left_pressed(void);

/** 左侧功能键按住时长（毫秒） */
uint32_t touch_left_hold_ms(void);

/** 顶部滑块位置：-1.0（最左）到 +1.0（最右） */
float touch_top_position(void);

/** 顶部滑块未平滑质心（滑动检测用 — 平滑值会低估快速位移） */
float touch_top_position_raw(void);

/** 顶部滑条(通道 1-5)是否有手指触摸 */
bool touch_is_top_pressed(void);

/** 右侧滑块位置：-1.0（最下）到 +1.0（最上） */
float touch_right_position(void);

/** 右侧是否有手指触摸 */
bool touch_is_right_pressed(void);

/** 检测到"抚摸头部"手势（>=3 个顶部通道超过 500ms） */
bool touch_is_petting_head(void);

/** 顶部滑条中间三电极 (GPIO4/5/6) 任一被触摸 */
bool touch_is_top_middle_pressed(void);

/** 获取原始触摸读数（用于校准/调试） */
void touch_get_raw(uint32_t *raw_out);
/** 获取基线值 */
void touch_get_baseline(uint32_t *bl_out);
/** 获取滤波后的 delta 值 */
void touch_get_filtered(int *filtered_out);
/** 获取当前各通道判定阈值 (诊断: 观察抬阈/死区) */
void touch_fpc_get_thr(int *thr_out);

/** 息屏暂停扫描定时器 — FreeRTOS 软件定时器 20ms 周期会限死轻睡
 * 窗口 (睡眠窗口 20ms→500ms)。息屏唤醒由模块内独立探针任务负责
 * (esp_timer 节拍, 20Hz 快探 / 2Hz 深闲, 见 probe_task_fn)。 */
void touch_fpc_pause(void);

/** 亮屏恢复扫描定时器: 重校准 (防手指污染守卫) + 50Hz 最高档起步。 */
void touch_fpc_resume(void);

/** 息屏期唤醒探针 (动态频率): 单发扫描 + 唤醒判定。
 * - 扫描侧判定不落盘 (基线恒追速, 尖峰当帧自回), 唤醒由逐通道独立
 *   复判 d > thr (无滞回无保活) 的 2 连击承担: 须落 150ms 窗内且同
 *   通道一致 — 尖峰单帧只计 1 次凑不齐, 供电瞬态跨通道轮换被作废
 * - USB/供电免疫窗 (5s) 内不判唤醒; 通道 >10 判环境态
 * 返回 true 后调用方亮屏。息屏时主循环以 touch_fpc_probe_interval_ms()
 * 返回的间隔调用 (快探 50ms / 深闲 500ms; 由模块内独立任务按
 * esp_timer 节拍驱动)。 */
bool touch_fpc_sleep_probe(void);

/** main.c 消费: 探针 2 连击达成置位 → 返回即清除。息屏期主循环
 * 轮询此接口, true → 走唤醒流程 (屏幕状态机一体化)。 */
bool touch_fpc_wake_pending(void);

/** 当前息屏探针间隔 (ms): 近场/触摸活动后 50ms (20Hz 快探), 无活动
 * 15s 后 500ms (2Hz 深闲) — main.c 据此调主循环睡眠窗口。 */
uint32_t touch_fpc_probe_interval_ms(void);

/** : USB/供电事件通知 — 拔插 USB (充电状态翻转)、U盘模式切换会
 * 引起供电链路瞬态 (VBUS 消失/恢复、PHY 电源域切换), raw 可跳几千且
 * 持续 1-2s, 探针 2 连击去抖会被穿透 → 假唤醒。调用后探针进入 5s 免疫窗
 * (不判唤醒) + 复位到快探档。手动唤醒路径 (左键/摇动/1s 兜底接触轮询)
 * 不受影响。由 usb_storage (模式切换) 与 main.c (充电翻转) 调用。 */
void touch_fpc_note_usb_event(void);

/** 诊断: 探针去抖命中数 (0..PROBE_HIT_REQUIRED-1) — CSV ph 列 */
uint8_t touch_fpc_probe_hits(void);

/** 诊断: 当前超阈值通道数 (0..12) — CSV tc 列 */
uint8_t touch_fpc_touched_count(void);

#ifdef __cplusplus
}
#endif

#endif /* TOUCH_FPC_H */
