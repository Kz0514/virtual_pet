/**
 * @file bq27220.c
 * @brief BQ27220 电池电量计 (I2C 0x55, 专有协议, 小端序)
 *
 * 数据手册: ZHCUAN8A (BQ27220 Technical Reference Manual)
 *
 * 标准命令 (2字节, 低地址=低字节, 小端序):
 *   0x06/07 Temperature()        0x2A/2B CycleCount()
 *   0x08/09 Voltage()            0x2C/2D StateOfCharge()
 *   0x0C/0D Current()            0x2E/2F StateOfHealth()
 *   0x10/11 RemainingCapacity()  0x3C/3D DesignCapacity()
 *   0x12/13 FullChargeCapacity() 0x7C/7D RawVoltage()
 */
#include "board.h"
#include "bq27220.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include <string.h>

static const char *TAG = "bq27220";
static i2c_master_dev_handle_t s_dev = NULL;
#define BATTERY_DESIGN_CAPACITY_MAH  800   /* 实际电池容量 */

/* ── BQ27220 Read Word (小端序) ── */
static esp_err_t reg_read(uint8_t cmd, uint16_t *value)
{
    uint8_t data[2];
    ESP_RETURN_ON_ERROR(
        i2c_master_transmit_receive(s_dev, &cmd, 1, data, 2, 100),
        TAG, "I2C读 cmd=0x%02X", cmd);
    *value = (uint16_t)data[0] | ((uint16_t)data[1] << 8);  /* 小端 */
    return ESP_OK;
}

esp_err_t bq27220_init(void)
{
    ESP_LOGI(TAG, "初始化 BQ27220 (I2C 0x55)…");

    i2c_master_bus_handle_t bus = board_get_i2c_bus();
    if (!bus) { ESP_LOGE(TAG, "I2C总线未就绪"); return ESP_ERR_INVALID_STATE; }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BQ27220_I2C_ADDR,
        .scl_speed_hz    = I2C_MASTER_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "I2C注册失败");

    uint16_t voltage;
    if (reg_read(0x08, &voltage) == ESP_OK) {
        ESP_LOGI(TAG, "BQ27220 就绪, 电压=%umV", voltage);
    }
    return ESP_OK;
}

esp_err_t bq27220_read(bq27220_data_t *out)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    uint16_t val;

    /* 电压 (0x08/09) */
    if (reg_read(0x08, &val) == ESP_OK) out->voltage_mv = val;
    else out->voltage_mv = 0;

    /* 电流 (0x0C/0D), 有符号 mA, +充电/-放电 */
    if (reg_read(0x0C, &val) == ESP_OK) out->current_ma = (int16_t)val;
    else out->current_ma = 0;

    /* SOC (0x2C/2D), % */
    if (reg_read(0x2C, &val) == ESP_OK) out->soc_pct = val;
    else out->soc_pct = 0;

    /* 满充容量 — 使用实际电池容量 */
    out->full_mah = BATTERY_DESIGN_CAPACITY_MAH;

    /* 剩余容量 — 根据 SOC% 计算 */
    out->remain_mah = (uint16_t)((uint32_t)out->soc_pct * BATTERY_DESIGN_CAPACITY_MAH / 100);

    /* 温度 (0x06/07), 0.1K → °C */
    if (reg_read(0x06, &val) == ESP_OK) out->temp_c = val / 10.0f - 273.15f;
    else out->temp_c = 25.0f;

    /* 健康度 (0x2E/2F), % */
    if (reg_read(0x2E, &val) == ESP_OK) out->health_pct = val;
    else out->health_pct = 100;

    /* 循环次数 (0x2A/2B) */
    if (reg_read(0x2A, &val) == ESP_OK) out->cycle_count = val;
    else out->cycle_count = 0;

    return ESP_OK;
}

esp_err_t bq27220_read_soc(uint16_t *soc_pct)
{
    return reg_read(0x2C, soc_pct);
}

/* ══════════════════════════════════════════════════════════════════════
 * 调试: BQ27220 全寄存器扫描 (按手册命令集)
 * ══════════════════════════════════════════════════════════════════════ */
void bq27220_debug_scan(void)
{
    if (!s_dev) { ESP_LOGE(TAG, "未初始化, 无法扫描"); return; }

    ESP_LOGI(TAG, "══════ BQ27220 寄存器扫描 (TRM命令集) ══════");

    static const struct {
        uint8_t cmd; const char *name; const char *fmt;
    } cmds[] = {
        {0x06, "Temperature",        "%.1fC"},
        {0x08, "Voltage",            "%umV"},
        {0x0C, "Current",            "%dmA"},
        {0x10, "RemainingCapacity",  "%umAh"},
        {0x12, "FullChargeCapacity", "%umAh"},
        {0x16, "TimeToEmpty",        "%umin"},
        {0x18, "TimeToFull",         "%umin"},
        {0x1A, "StandbyCurrent",     "%dmA"},
        {0x1C, "StandbyTimeToEmpty", "%umin"},
        {0x1E, "MaxLoadCurrent",     "%dmA"},
        {0x20, "MaxLoadTimeToEmpty", "%umin"},
        {0x22, "RawCoulombCount",    "%u"},
        {0x24, "AveragePower",       "%umW"},
        {0x28, "InternalTemp",       "%.1fC"},
        {0x2A, "CycleCount",         "%u"},
        {0x2C, "StateOfCharge",      "%u%%"},
        {0x2E, "StateOfHealth",      "%u%%"},
        {0x30, "ChargingVoltage",    "%umV"},
        {0x32, "ChargingCurrent",    "%dmA"},
        {0x34, "BTPDischargeSet",    "%umV"},
        {0x36, "BTPChargeSet",       "%umV"},
        {0x3A, "OperationStatus",    "0x%04X"},
        {0x3C, "DesignCapacity",     "%umAh"},
        {0x79, "AnalogCount",        "%u"},
        {0x7A, "RawCurrent",         "%d"},
        {0x7C, "RawVoltage",         "%umV"},
    };

    int found = 0;
    for (int i = 0; i < (int)(sizeof(cmds)/sizeof(cmds[0])); i++) {
        uint16_t val;
        if (reg_read(cmds[i].cmd, &val) != ESP_OK) {
            ESP_LOGW(TAG, "  0x%02X %-20s  NACK/超时", cmds[i].cmd, cmds[i].name);
            continue;
        }
        found++;

        char buf[64];
        const char *fmt = cmds[i].fmt;
        if      (strstr(fmt, "C"))   snprintf(buf, sizeof(buf), fmt, val / 10.0f - 273.15f);
        else if (strstr(fmt, "mA"))  snprintf(buf, sizeof(buf), fmt, (int16_t)val);
        else if (strstr(fmt, "X"))   snprintf(buf, sizeof(buf), fmt, val);
        else                         snprintf(buf, sizeof(buf), fmt, val);

        ESP_LOGI(TAG, "✓ 0x%02X %-20s = %s", cmds[i].cmd, cmds[i].name, buf);
    }
    ESP_LOGI(TAG, "══════ 扫描完成: %d/27 个寄存器响应 ══════", found);
}
