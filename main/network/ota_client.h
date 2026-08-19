#pragma once
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Check for firmware update and apply if available. Returns true if updated. */
bool ota_client_check_and_update(const char *token, const char *current_version);

/** FreeRTOS task entry for OTA check (doesn't block main loop) */
void ota_check_task(void *pvParameter);

#ifdef __cplusplus
}
#endif
