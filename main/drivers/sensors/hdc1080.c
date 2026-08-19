/**
 * @file hdc1080.c
 * @brief HDC1080 温湿度传感器驱动 (I2C 0x40, 14位分辨率)
 *
 * 转换时间 ~6.5ms, 温度精度 ±0.4°C, 湿度精度 ±2%RH
 */
#include "board.h"
#include "hdc1080.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hdc1080";

#define HDC1080_REG_TEMP     0x00
#define HDC1080_REG_HUMID    0x01
#define HDC1080_REG_CONFIG   0x02
#define HDC1080_REG_DEV_ID   0xFF
#define HDC1080_DEVICE_ID    0x1050

static i2c_master_dev_handle_t s_dev = NULL;

esp_err_t hdc1080_init(void)
{
    ESP_LOGI(TAG, "初始化 HDC1080 (I2C 0x40)…");

    i2c_master_bus_handle_t bus = board_get_i2c_bus();
    if (!bus) { ESP_LOGE(TAG, "I2C 总线未就绪"); return ESP_ERR_INVALID_STATE; }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = HDC1080_I2C_ADDR,
        .scl_speed_hz    = I2C_MASTER_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "I2C设备注册失败");

    /* 验证设备ID */
    uint8_t id[2];
    if (i2c_master_transmit_receive(s_dev, (uint8_t[]){HDC1080_REG_DEV_ID}, 1, id, 2, 100) == ESP_OK) {
        uint16_t dev_id = ((uint16_t)id[0] << 8) | id[1];
        ESP_LOGI(TAG, "设备ID: 0x%04X (期望 0x%04X)", dev_id, HDC1080_DEVICE_ID);
    }

    /* 配置: 温湿度都测量, 14位分辨率 */
    uint8_t cfg_buf[3] = { HDC1080_REG_CONFIG, 0x00, 0x00 };
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_dev, cfg_buf, 3, 100), TAG, "配置写入失败");

    ESP_LOGI(TAG, "HDC1080 就绪");
    return ESP_OK;
}

esp_err_t hdc1080_read(hdc1080_data_t *out)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    uint8_t raw[2];

    /* 温度: 写指针0x00 → 等9ms → 读2字节 */
    uint8_t reg = HDC1080_REG_TEMP;
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_dev, &reg, 1, 100), TAG, "触发测温失败");
    vTaskDelay(pdMS_TO_TICKS(9));
    ESP_RETURN_ON_ERROR(i2c_master_receive(s_dev, raw, 2, 100), TAG, "读温度失败");
    out->temperature = (float)(((uint16_t)raw[0] << 8) | raw[1]) / 65536.0f * 165.0f - 40.0f;

    /* 湿度: 写指针0x01 → 等9ms → 读2字节 */
    reg = HDC1080_REG_HUMID;
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_dev, &reg, 1, 100), TAG, "触发测湿失败");
    vTaskDelay(pdMS_TO_TICKS(9));
    ESP_RETURN_ON_ERROR(i2c_master_receive(s_dev, raw, 2, 100), TAG, "读湿度失败");
    out->humidity = (float)(((uint16_t)raw[0] << 8) | raw[1]) / 65536.0f * 100.0f;

    return ESP_OK;
}
