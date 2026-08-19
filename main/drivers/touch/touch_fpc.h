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

#ifdef __cplusplus
}
#endif

#endif /* TOUCH_FPC_H */
