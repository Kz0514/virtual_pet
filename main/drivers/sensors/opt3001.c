/**
 * @file opt3001.c
 * @brief OPT3001-Q1 环境光传感器驱动 (I2C 0x44, 自动量程)
 *
 * 数据格式: [15:12]=指数E, [11:0]=尾数M
 * 照度 = M × 2^E × 0.01 lux
 */
#include "board.h"
#include "opt3001.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "opt3001";

#define OPT3001_REG_RESULT      0x00
#define OPT3001_REG_CONFIG      0x01
#define OPT3001_REG_MANUF_ID    0x7E
#define OPT3001_REG_DEVICE_ID   0x7F
#define OPT3001_MANUF_ID        0x5449
#define OPT3001_DEVICE_ID       0x3001

static i2c_master_dev_handle_t s_dev = NULL;

esp_err_t opt3001_init(void)
{
    ESP_LOGI(TAG, "初始化 OPT3001 (I2C 0x44)…");

    i2c_master_bus_handle_t bus = board_get_i2c_bus();
    if (!bus) { ESP_LOGE(TAG, "I2C 总线未就绪"); return ESP_ERR_INVALID_STATE; }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = OPT3001_I2C_ADDR,
        .scl_speed_hz    = I2C_MASTER_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "I2C设备注册失败");

    /* 验证设备ID */
    uint8_t id[2];
    if (i2c_master_transmit_receive(s_dev, (uint8_t[]){OPT3001_REG_DEVICE_ID}, 1, id, 2, 100) == ESP_OK) {
        uint16_t dev_id = ((uint16_t)id[0] << 8) | id[1];
        ESP_LOGI(TAG, "设备ID: 0x%04X (期望 0x%04X)", dev_id, OPT3001_DEVICE_ID);
    }

    /* 配置: 连续测量, 自动量程, 转换时间800ms */
    /* CONFIG[11:9]=110(连续), [5:4]=11(自动量程) → 0xC610 */
    uint8_t cfg_buf[3] = { OPT3001_REG_CONFIG, 0xCE, 0x10 };
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_dev, cfg_buf, 3, 100), TAG, "配置写入失败");

    ESP_LOGI(TAG, "OPT3001 就绪 (首次读数需 ~800ms)");
    return ESP_OK;
}

esp_err_t opt3001_read_lux(float *lux)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;

    uint8_t raw[2];
    uint8_t reg = OPT3001_REG_RESULT;
    ESP_RETURN_ON_ERROR(
        i2c_master_transmit_receive(s_dev, &reg, 1, raw, 2, 100),
        TAG, "读结果失败");

    uint16_t val = ((uint16_t)raw[0] << 8) | raw[1];
    int exponent = (val >> 12) & 0x0F;
    int mantissa = val & 0x0FFF;
    *lux = (float)mantissa * (1 << exponent) * 0.01f;

    return ESP_OK;
}
