/** @file asset_client.h @brief SPIFFS 资源 OTA (动画帧等) */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 检查服务器资源列表, 下载缺失/更新的文件到 SPIFFS.
 * 应在 WiFi 连接后、pet_avatar 初始化前调用.
 * @param token  JWT token 用于认证
 */
void asset_update_task(void *pvParameter);

#ifdef __cplusplus
}
#endif
