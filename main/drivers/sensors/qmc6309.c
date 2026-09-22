/**
 * @file qmc6309.c
 * @brief QMC6309 3轴地磁传感器驱动 (指南针方向)
 *
 * 硬件特殊性: QMC6309 不直连 I2C0. 挂载在 MPU6500 的 AUX I2C 总线上,
 * MPU6500 初始化时使能 INT_PIN_CFG.BYPASS_EN 后, 可通过主 I2C0 直接访问 (地址 0x7C).
 *
 * 寄存器参考: QMC6309 Datasheet (QST) Rev A
 *   - 0x00:      Chip ID = 0x90
 *   - 0x01-0x06: X/Y/Z 数据 (16-bit, 小端, 有符号)
 *   - 0x09:      状态 (只读)
 *   - 0x0A:      Control 1 — OSR2[7:5] / OSR1[4:3] / MODE[1:0]
 *   - 0x0B:      Control 2 — SOFT_RST[7] / ODR[6:4] / RNG[3:2] / SET_RESETMODE[1:0]
 */

#include "board.h"
#include "qmc6309.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

static const char *TAG = "qmc6309";

#define QMC6309_ADDR     0x7C

/* 寄存器 */
#define REG_CHIP_ID      0x00
#define REG_X_LSB        0x01  /* 0x01-0x06: X/Y/Z 各 2 字节 */
#define REG_CTRL1        0x0A
#define REG_CTRL2        0x0B

#define QMC6309_CHIP_ID  0x90
#define QMC6309_SOFT_RST 0x80

/* Control 1: OSR2=011(8), OSR1=00(8), MODE=01(Normal) */
#define CTRL1_CFG        0x61
/* Control 2: ODR=011(100Hz), RNG=10(±8G), SET/RESETMODE=00(Set and reset on) */
#define CTRL2_CFG        0x38

/* 灵敏度: ±8G → 4000 LSB/Gauss → 40 LSB/μT */
#define QMC6309_SENSITIVITY  40.0f

static i2c_master_dev_handle_t s_dev = NULL;

/* ── I2C 辅助 ── */
static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, 2, 10);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *dst, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, dst, len, 20);
}

/* ════════════════════════════════════════════════════════════════ */

esp_err_t qmc6309_init(void)
{
    ESP_LOGI(TAG, "初始化 QMC6309 (I2C 0x%02X, 经MPU6500 AUX旁路)…", QMC6309_ADDR);

    /* 获取 I2C 总线 */
    i2c_master_bus_handle_t bus = board_get_i2c_bus();
    if (!bus) { ESP_LOGE(TAG, "I2C总线未就绪"); return ESP_ERR_INVALID_STATE; }

    /* 注册 I2C 设备 */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = QMC6309_ADDR,
        .scl_speed_hz    = I2C_MASTER_FREQ_HZ,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C设备注册失败: %d — AUX旁路是否已启用?", ret);
        return ret;
    }

    /* 芯片身份 */
    uint8_t chip_id = 0;
    ret = reg_read(REG_CHIP_ID, &chip_id, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Chip ID 读取失败: %d — 检查 AUX 旁路和接线", ret);
        return ret;
    }
    if (chip_id != QMC6309_CHIP_ID) {
        ESP_LOGE(TAG, "Chip ID = 0x%02X, 预期 0x%02X", chip_id, QMC6309_CHIP_ID);
        return ESP_ERR_NOT_FOUND;
    }

    /* 软复位: SOFT_RST 不自清, 必须再写一次 0x00 */
    reg_write(REG_CTRL2, QMC6309_SOFT_RST);
    vTaskDelay(pdMS_TO_TICKS(10));
    reg_write(REG_CTRL2, 0x00);
    vTaskDelay(pdMS_TO_TICKS(10));

    reg_write(REG_CTRL2, CTRL2_CFG);   /* ODR / 量程 / Set-Reset */
    reg_write(REG_CTRL1, CTRL1_CFG);   /* OSR + 模式 */
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t ctrl1 = 0;
    if (reg_read(REG_CTRL1, &ctrl1, 1) == ESP_OK)
        ESP_LOGI(TAG, "CTRL1=0x%02X MODE=%d (1=Normal)", ctrl1, ctrl1 & 0x03);
    ESP_LOGI(TAG, "QMC6309 初始化完成 (100Hz, ±8G, OSR 8/8)");
    return ESP_OK;
}

/* ════════════════════════════════════════════════════════════════ */

esp_err_t qmc6309_read(qmc6309_data_t *data)
{
    if (!s_dev || !data) return ESP_ERR_INVALID_STATE;
    memset(data, 0, sizeof(*data));

    /* 读取 6 字节: X_L, X_H, Y_L, Y_H, Z_L, Z_H (小端, 有符号) */
    uint8_t raw[6];
    esp_err_t ret = reg_read(REG_X_LSB, raw, 6);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C读取失败: %d", ret);
        return ret;
    }

    int16_t mx = (int16_t)((raw[1] << 8) | raw[0]);
    int16_t my = (int16_t)((raw[3] << 8) | raw[2]);
    int16_t mz = (int16_t)((raw[5] << 8) | raw[4]);

    data->x = mx / QMC6309_SENSITIVITY;
    data->y = my / QMC6309_SENSITIVITY;
    data->z = mz / QMC6309_SENSITIVITY;

    /* 方位角: atan2(y, x) 弧度转度, 0° = 北; 未做倾斜补偿, 仅在水平放置时成立 */
    float heading_rad = atan2f(data->y, data->x);
    data->heading = heading_rad * 180.0f / (float)M_PI;
    if (data->heading < 0) data->heading += 360.0f;

    return ESP_OK;
}
