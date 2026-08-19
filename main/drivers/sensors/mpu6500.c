/**
 * MPU-6500 6-axis IMU driver (I2C 0x68, Register Map Rev 2.1)
 *
 * Accel: ±4g (8192 LSB/g), Gyro: ±250dps (131 LSB/dps), 200Hz ODR
 */
#include "board.h"
#include "mpu6500.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

static const char *TAG = "mpu6500";
static i2c_master_dev_handle_t s_dev = NULL;

/* ── Register addresses (MPU-6500 mode) ── */
enum {
    REG_SMPLRT_DIV   = 0x19,
    REG_CONFIG       = 0x1A,
    REG_GYRO_CONFIG  = 0x1B,
    REG_ACCEL_CONFIG = 0x1C,
    REG_ACCEL_CFG2   = 0x1D,
    REG_INT_PIN_CFG  = 0x37,
    REG_INT_ENABLE   = 0x38,
    REG_INT_STATUS   = 0x3A,
    REG_ACCEL_XOUT_H = 0x3B,
    REG_USER_CTRL    = 0x6A,
    REG_PWR_MGMT_1   = 0x6B,
    REG_PWR_MGMT_2   = 0x6C,
    REG_FIFO_EN      = 0x23,
    REG_FIFO_COUNT_H = 0x72,
    REG_FIFO_COUNT_L = 0x73,
    REG_FIFO_R_W     = 0x74,
    REG_WHO_AM_I     = 0x75,

    /* MPU-6500 accel offsets (15-bit, NOT 6050-compatible 6-11) */
    REG_XA_OFFS_H    = 0x77,
    REG_XA_OFFS_L    = 0x78,
    REG_YA_OFFS_H    = 0x7A,
    REG_YA_OFFS_L    = 0x7B,
    REG_ZA_OFFS_H    = 0x7D,
    REG_ZA_OFFS_L    = 0x7E,
};

static esp_err_t wr(uint8_t reg, uint8_t val) {
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(s_dev, b, 2, 10);
}
static esp_err_t rd(uint8_t reg, uint8_t *dst, size_t len) {
    return i2c_master_transmit_receive(s_dev, &reg, 1, dst, len, 20);
}

/* ════════════════════════════════════════════════════════════════ */
esp_err_t mpu6500_init(void)
{
    ESP_LOGI(TAG, "Init MPU6500 (0x%02X)…", MPU6500_I2C_ADDR);

    i2c_master_bus_handle_t bus = board_get_i2c_bus();
    if (!bus) return ESP_ERR_INVALID_STATE;

    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = MPU6500_I2C_ADDR,
        .scl_speed_hz    = I2C_MASTER_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dc, &s_dev), TAG, "I2C dev");

    /* WHO_AM_I */
    uint8_t who;
    rd(REG_WHO_AM_I, &who, 1);
    ESP_LOGI(TAG, "WHO_AM_I = 0x%02X (expect 0x70)", who);

    /* 1. Device reset */
    wr(REG_PWR_MGMT_1, 0x80);
    vTaskDelay(pdMS_TO_TICKS(100));
    uint8_t pm;
    for (int i = 0; i < 20; i++) { rd(REG_PWR_MGMT_1, &pm, 1); if (!(pm & 0x80)) break; vTaskDelay(5); }

    /* 2. Wake + auto clock */
    wr(REG_PWR_MGMT_1, 0x01);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* 3. ODR = 1000/(1+4) = 200Hz */
    wr(REG_SMPLRT_DIV, 0x04);

    /* 4. Gyro DLPF: 20Hz bandwidth, Fs=1kHz (FCHOICE_B=00 requires DLPF) */
    wr(REG_CONFIG, 0x04);

    /* 5. Gyro: ±250dps, FCHOICE_B=00 (use DLPF) */
    wr(REG_GYRO_CONFIG, 0x00);

    /* 6. Accel: ±4g */
    wr(REG_ACCEL_CONFIG, 0x08);

    /* 7. Accel DLPF: 20Hz bandwidth (A_DLPF_CFG=4, FCHOICE_B=0) */
    wr(REG_ACCEL_CFG2, 0x04);

    /* 8. INT: AUX bypass for QMC6309 */
    wr(REG_INT_PIN_CFG, 0x02);
    wr(REG_INT_ENABLE, 0x01);

    /* 9. FIFO: accel + gyro + temp, 14 bytes/sample, 36 samples capacity */
    wr(REG_USER_CTRL, 0x44);   /* FIFO_EN + FIFO_RST */
    wr(REG_FIFO_EN, 0xF8);     /* TEMP + GYRO_XYZ + ACCEL */
    wr(REG_USER_CTRL, 0x40);   /* FIFO_EN (clear reset) */

    /* Print config summary */
    uint8_t r[7];
    rd(REG_PWR_MGMT_1,   r+0, 1);
    rd(REG_SMPLRT_DIV,   r+1, 1);
    rd(REG_CONFIG,       r+2, 1);
    rd(REG_GYRO_CONFIG,  r+3, 1);
    rd(REG_ACCEL_CONFIG, r+4, 1);
    rd(REG_ACCEL_CFG2,   r+5, 1);
    rd(REG_INT_PIN_CFG,  r+6, 1);
    ESP_LOGI(TAG, "Regs: PWR=%02X SMPL=%02X CFG=%02X GYRO=%02X ACC=%02X ACFG2=%02X INT=%02X",
             r[0], r[1], r[2], r[3], r[4], r[5], r[6]);

    ESP_LOGI(TAG, "Ready (±4g, ±250dps, 200Hz)");
    return ESP_OK;
}

/* ════════════════════════════════════════════════════════════════ */
esp_err_t mpu6500_read(mpu6500_data_t *data)
{
    if (!s_dev || !data) return ESP_ERR_INVALID_STATE;
    memset(data, 0, sizeof(*data));

    uint8_t raw[14];
    esp_err_t ret = rd(REG_ACCEL_XOUT_H, raw, 14);
    if (ret != ESP_OK) return ret;

    int16_t ax = (int16_t)((raw[0] << 8) | raw[1]);
    int16_t ay = (int16_t)((raw[2] << 8) | raw[3]);
    int16_t az = (int16_t)((raw[4] << 8) | raw[5]);
    data->accel_x = ax / 8192.0f;
    data->accel_y = ay / 8192.0f;
    data->accel_z = az / 8192.0f;

    int16_t tr = (int16_t)((raw[6] << 8) | raw[7]);
    data->temperature = tr / 333.87f + 21.0f;

    int16_t gx = (int16_t)((raw[8]  << 8) | raw[9]);
    int16_t gy = (int16_t)((raw[10] << 8) | raw[11]);
    int16_t gz = (int16_t)((raw[12] << 8) | raw[13]);
    data->gyro_x = gx / 131.0f;
    data->gyro_y = gy / 131.0f;
    data->gyro_z = gz / 131.0f;

    return ESP_OK;
}

/* ════════════════════════════════════════════════════════════════ */

i2c_master_dev_handle_t mpu6500_get_i2c_dev(void) { return s_dev; }

void mpu6500_debug_scan(void)
{
    if (!s_dev) { ESP_LOGE(TAG, "Not inited"); return; }

    uint8_t raw[14];
    if (rd(REG_ACCEL_XOUT_H, raw, 14) != ESP_OK) { ESP_LOGE(TAG, "Read fail"); return; }

    ESP_LOGI(TAG, "══════ MPU6500 Scan ══════");
    ESP_LOGI(TAG, "Raw bytes: %02X%02X %02X%02X %02X%02X | %02X%02X | %02X%02X %02X%02X %02X%02X",
             raw[0],raw[1], raw[2],raw[3], raw[4],raw[5], raw[6],raw[7],
             raw[8],raw[9], raw[10],raw[11], raw[12],raw[13]);

    /* Read accel offsets */
    uint8_t xoh, xol, yoh, yol, zoh, zol;
    rd(REG_XA_OFFS_H, &xoh, 1); rd(REG_XA_OFFS_L, &xol, 1);
    rd(REG_YA_OFFS_H, &yoh, 1); rd(REG_YA_OFFS_L, &yol, 1);
    rd(REG_ZA_OFFS_H, &zoh, 1); rd(REG_ZA_OFFS_L, &zol, 1);
    int16_t xo = (int16_t)(((xoh << 7) | (xol >> 1)) << 1);  /* 15-bit sign-extend */
    int16_t yo = (int16_t)(((yoh << 7) | (yol >> 1)) << 1);
    int16_t zo = (int16_t)(((zoh << 7) | (zol >> 1)) << 1);
    ESP_LOGI(TAG, "Offsets: X=%d Y=%d Z=%d", xo, yo, zo);

    ESP_LOGI(TAG, "══ Data ══");
    int16_t ax = (int16_t)((raw[0] << 8) | raw[1]);
    int16_t ay = (int16_t)((raw[2] << 8) | raw[3]);
    int16_t az = (int16_t)((raw[4] << 8) | raw[5]);
    int16_t gx = (int16_t)((raw[8]  << 8) | raw[9]);
    int16_t gy = (int16_t)((raw[10] << 8) | raw[11]);
    int16_t gz = (int16_t)((raw[12] << 8) | raw[13]);
    ESP_LOGI(TAG, "Accel raw: X=%d Y=%d Z=%d", ax, ay, az);
    ESP_LOGI(TAG, "Accel:     X=%+.3f Y=%+.3f Z=%+.3f g", ax/8192.0f, ay/8192.0f, az/8192.0f);
    ESP_LOGI(TAG, "Gyro:      X=%+.2f Y=%+.2f Z=%+.2f dps", gx/131.0f, gy/131.0f, gz/131.0f);
    ESP_LOGI(TAG, "|a|:       %.3f g", sqrtf((ax/8192.0f)*(ax/8192.0f)+(ay/8192.0f)*(ay/8192.0f)+(az/8192.0f)*(az/8192.0f)));
}
