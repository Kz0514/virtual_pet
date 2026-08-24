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

/** 息屏准备硬件触摸唤醒 (v2): 按软件基线把硬件唤醒阈值调到轻触可及
 * (S3 硬件比较器 = raw − 硬件基准 > 阈值; 出厂 800 对轻触 delta ~150-300
 * 太钝 — 初按压实测最大 ~150)。同时打印 硬件基准 vs 软件基线 对比,
 * 验证比较器基准口径。FSM 自 init 起常跑, 轻睡时硬件继续采样。 */
void touch_fpc_prepare_wakeup(void);

/** 息屏暂停 50Hz 扫描定时器 — FreeRTOS 软件定时器 20ms 周期会限死轻睡
 * 窗口 (睡眠窗口 20ms→1s)。硬件 FSM 不受影响 (唤醒靠硬件比较器)。 */
void touch_fpc_pause(void);

/** 亮屏恢复 50Hz 扫描定时器 (读数缓存恢复 → 手势/滑动检测回到正常)。 */
void touch_fpc_resume(void);

/** 1Hz 兜底单发扫描 : 息屏期扫描定时器已停, 硬件触摸唤醒依赖
 * 轻睡真正进入 — 轻睡不生效时 (USB 插着等) 触摸无消费者。主循环 1s 块
 * 调用, 保持 filtered/touched 新鲜 → contacted 检查 (filtered>250) 即可
 * 唤醒, 无论轻睡是否生效。 */
void touch_fpc_poll_once(void);

/** 息屏期唤醒探针 ( 简化, 10Hz): 单发扫描 + 唤醒判定。
 * - 复用触摸模块共享判定 (s_ts.touched — raw vs 模块基线, any_active
 * 门禁 1/32 EMA), 与亮屏触摸同一套数据 — 睡眠侧不再维护专用动态
 * 基线 (v2.15-2.17 的 s_sleep_base 已删, 用户拍板调优归触摸模块)
 * - 连续 3 次 (300ms) 命中才唤醒; 超阈值通道 >10 判环境态
 * - v2.16: USB/供电事件免疫窗内不判唤醒 (拔插瞬态)
 * 返回 true 后调用方亮屏。息屏时主循环 100ms 块调用。 */
bool touch_fpc_sleep_probe(void);

/** : USB/供电事件通知 — 拔插 USB (充电状态翻转)、U盘模式切换会
 * 引起供电链路瞬态 (VBUS 消失/恢复、PHY 电源域切换), raw 可跳几千且
 * 持续 1-2s, 探针 3 次去抖会被穿透 → 假唤醒。调用后探针进入 5s 免疫窗
 * (不判唤醒)。手动唤醒路径 (左键/摇动/1s 兜底接触轮询) 不受影响。
 * 由 usb_storage (模式切换) 与 main.c (充电翻转) 调用。 */
void touch_fpc_note_usb_event(void);

/** 诊断 : 探针去抖命中数 (0..PROBE_HIT_REQUIRED-1) — CSV ph 列 */
uint8_t touch_fpc_probe_hits(void);

/** 诊断 : 当前超阈值通道数 (0..12) — CSV tc 列 */
uint8_t touch_fpc_touched_count(void);

#ifdef __cplusplus
}
#endif

#endif /* TOUCH_FPC_H */
