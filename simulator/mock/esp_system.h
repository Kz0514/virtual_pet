/** @file mock/esp_system.h — Mock ESP-IDF 系统 API */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 设置页"重启设备"→ 模拟器直接退出 (实现见 mock_impl.c) */
void esp_restart(void);

#ifdef __cplusplus
}
#endif
