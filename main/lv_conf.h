/**
 * @file lv_conf.h
 * @brief LVGL v9 配置 (FS 驱动在 sdkconfig 中设置)
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
#define LV_MEM_SIZE (48 * 1024)
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_DISP_DEF_REFR_PERIOD 30
#define LV_USE_INDEV 1
#define LV_USE_GPU 0
/* LV_USE_FS 在 v9 已废弃, 各 FS 驱动独立开关, 见 sdkconfig */
/* 系统 malloc — 大块分配走 PSRAM, 不限 64KB 内部池 */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* 注意: 真机 sdkconfig 中 CONFIG_LV_CONF_SKIP=y, 本文件不被真机编译使用!
 * 真机 LVGL 配置走 sdkconfig Kconfig 选项.
 * 大字库支持在真机对应 sdkconfig: CONFIG_LV_FONT_FMT_TXT_LARGE=y
 * 此处仅对使用本文件的构建 (如模拟器参考) 有效 */
#define LV_FONT_FMT_TXT_LARGE 1

/* CJK Chinese font — disabled, using custom zh.c instead */
// #define LV_USE_FONT_SOURCE_HAN_SANS_SC_14_CJK 1

#endif
