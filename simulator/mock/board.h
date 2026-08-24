/** @file mock/board.h — Mock 硬件板级定义 (遮蔽 main/board.h)
 *
 * 真 board.h include ESP-IDF 专用头 (hal/driver), PC 编译不过。
 * 模拟器只用到低电量阈值宏 (status_bar.c 电池图标变红)。
 * 因 include 路径顺序 mock/ 在 main/ 之前, 本文件遮蔽真 board.h。
 */
#pragma once

#define BATTERY_CRITICAL_THRESHOLD_PCT 5
