/**
 * @file qmc6309.c
 * @brief QMC6309 3轴地磁传感器驱动 (指南针方向)
 *
 * ⚠️ 硬件特殊性: QMC6309 不直连 I2C0 总线.
 * 挂载在 MPU6500 的 AUX I2C 总线上, 读取路径:
 *   ESP32 → I2C0 → MPU6500 (I2C_SLVx旁路) → AUX I2C → QMC6309 (地址 0x2C)
 *
 * MPU6500 初始化时使能 INT_PIN_CFG.BYPASS_EN 后, QMC6309 可通过主 I2C0 直接访问.
 *
 * 寄存器参考: QMC6309 Datasheet (QST Corporation)
 *   - 0x00-0x05: X/Y/Z 数据 (16-bit, 小端, 有符号)
 *   - 0x09: Control 1 (模式/ODR/量程/OSR)
 *   - 0x0A: Control 2 (中断/软复位)
 *   - 0x0D: WIA (Who I Am) = 0x31
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

#define QMC6309_ADDR     0x2C

/* 寄存器 */
#define REG_X_LSB        0x00
#define REG_CTRL1        0x09
#define REG_CTRL2        0x0A
#define REG_WIA          0x0D

/* 灵敏度: ±2G → 12000 LSB/Gauss → 120 LSB/μT */
#define QMC6309_SENSITIVITY  120.0f  /* LSB/μT (for ±2G range) */

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

    /* 读 Chip ID */
    uint8_t whoami = 0;
    if (reg_read(REG_WIA, &whoami, 1) == ESP_OK) {
        ESP_LOGI(TAG, "WHO_AM_I = 0x%02X (预期 0x31)", whoami);
    } else {
        ESP_LOGW(TAG, "WHO_AM_I 读取失败, 检查 AUX 旁路和接线");
    }

    /* 软复位 */
    reg_write(REG_CTRL2, 0x80);          /* SOFT_RST */
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Control 1: OSR=512, ±2G, 100Hz, 连续模式 */
    /* 0x0D = 0b00001101: OSR[7:5]=000(512), RNG[4:3]=00(±2G),
     *                      ODR[2:1]=10(100Hz), MODE[0]=1(连续) */
    reg_write(REG_CTRL1, 0x0D);
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t ctrl1;
    reg_read(REG_CTRL1, &ctrl1, 1);
    ESP_LOGI(TAG, "CTRL1=0x%02X, 模式=%s", ctrl1, (ctrl1 & 0x01) ? "连续" : "待机");
    ESP_LOGI(TAG, "QMC6309 初始化完成 (100Hz, ±2G, OSR512)");
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

    /* 方位角: atan2(y, x) 弧度转度, 0° = 北 */
    float heading_rad = atan2f(data->y, data->x);
    data->heading = heading_rad * 180.0f / (float)M_PI;
    if (data->heading < 0) data->heading += 360.0f;

    return ESP_OK;
}
