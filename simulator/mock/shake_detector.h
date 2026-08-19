/** @file mock/shake_detector.h — Mock 摇晃检测 */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline void shake_detector_suppress(bool on) { (void)on; }

#ifdef __cplusplus
}
#endif
