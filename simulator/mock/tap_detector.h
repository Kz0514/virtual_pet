/** @file mock/tap_detector.h — Mock 敲击检测 */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline void tap_detector_suppress(bool on) { (void)on; }

#ifdef __cplusplus
}
#endif
