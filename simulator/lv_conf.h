/**
 * @file lv_conf.h
 * @brief LVGL v9.5 配置 — PC 模拟器 (SDL2 后端)
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* ── 显示 ── */
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0       /* RGB565 byte order */

/* ── 内存 ── */
#define LV_MEM_SIZE (512 * 1024) /* 512KB for PC */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB

/* ── 刷新 ── */
#define LV_DISP_DEF_REFR_PERIOD 16  /* ~60fps */

/* ── 日志 ── */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_INFO

/* ── SDL2 后端 ── */
#define LV_USE_SDL 1
#define LV_USE_SDL_RENDERER 1    /* HW-accelerated renderer */

/* ── 输入设备 ── */
#define LV_USE_INDEV 1

/* ── 内存文件系统 (lv_binfont_create_from_buffer 需要) ── */
#define LV_USE_FS_MEMFS 1
#define LV_FS_MEMFS_LETTER 'M'

/* ── 大字库支持 ──
 * bitmap_index 默认 20 位 (最大 1MB 字形数据)!
 * zh_test.bin 有 2.7MB 字形, gid>8549 的字会溢出回绕导致乱码.
 * 实机 main/lv_conf.h 也必须加这一行才能用大字体! */
#define LV_FONT_FMT_TXT_LARGE 1

/* ── 功能开关 ── */
#define LV_USE_OBSERVER 0        /* Disable to save compile time */
#define LV_USE_SYSMON 0          /* Performance monitor off */

/* ── 字体 ── */
#define LV_FONT_DEFAULT &lv_font_montserrat_14
#define LV_FONT_MONTSERRAT_16 1    /* status_bar uses 16px */

/* ── 主题 ── */
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_GROW 1

/* ── Widgets ── */
#define LV_USE_LABEL 1
#define LV_USE_BTN 1
#define LV_USE_SLIDER 1
#define LV_USE_BAR 1
#define LV_USE_IMAGE 1
#define LV_USE_ANIM 1
#define LV_USE_SWITCH 1
#define LV_USE_SPINNER 1

/* ── Layouts ── */
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

/* ── GPU 加速 ── */
#define LV_USE_GPU 0

#endif /* LV_CONF_H */
