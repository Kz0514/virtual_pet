/** @file mock/st7789.h — Mock ST7789 display driver */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* brightness_bar.c 使用的背光控制 */
static inline void st7789_backlight_set(int brightness) {
    (void)brightness; /* no-op in simulator */
}

#ifdef __cplusplus
}
#endif
