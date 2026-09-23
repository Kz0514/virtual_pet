/**
 * ESP32 port layer for InvenSense DMP library.
 * Replaces STM32 HAL macros with ESP-IDF equivalents.
 */
#pragma once
#include <stdint.h>
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Set by dmp_mpu_init ── */
extern i2c_master_dev_handle_t dmp_i2c_dev;

/* ── Platform abstractions ── */
#define i2c_write(dev_addr, reg_addr, data_size, p_data) \
 dmp_i2c_write(reg_addr, data_size, p_data)

#define i2c_read(dev_addr, reg_addr, data_size, p_data) \
 dmp_i2c_read(reg_addr, data_size, p_data)

#define delay_ms(ms) vTaskDelay(pdMS_TO_TICKS(ms))
#define get_ms(p) do { *p = xTaskGetTickCount() * portTICK_PERIOD_MS; } while(0)
#define log_i(...) do {} while(0)
#define log_e(...) do {} while(0)

/* ── Implementation helpers ── */
static inline int dmp_i2c_write(uint8_t reg, uint8_t len, const uint8_t *data) {
 uint8_t buf[len + 1]; buf[0] = reg;
 for (int i = 0; i < len; i++) buf[i+1] = data[i];
 return i2c_master_transmit(dmp_i2c_dev, buf, len + 1, 100) == ESP_OK ? 0 : -1;
}
static inline int dmp_i2c_read(uint8_t reg, uint8_t len, uint8_t *data) {
 return i2c_master_transmit_receive(dmp_i2c_dev, &reg, 1, data, len, 100) == ESP_OK ? 0 : -1;
}

#ifdef __cplusplus
}
#endif
