#pragma once
#include "esp_err.h"
#include "lvgl.h"

esp_err_t ota_screen_init(void);
void ota_screen_show_progress(int percent, const char *status);
