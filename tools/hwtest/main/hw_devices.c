/**
 * @file hw_devices.c
 * @brief C 段 六器件 + F 段 音频通路
 *
 * 全部走 raw i2c_master (不依赖 app 驱动) —— 测的是板子和器件, 不是 app 的抽象层。
 * 器件初始化序列抄自各路驱动的对应行 (每处注明来源), 硬件改版时对账这两边。
 *
 * 已知测试假象 (i2c-bus-dead-board-2026-09 记录, 别当器件故障):
 *   ① HDC1080: i2c_master_transmit_receive(0x40, 0x00, 2) 在**两块板**上都 100% INVALID
 *      ⇒ 该读法本身不合法; 正确读法 = 写指针 → 延时 → 纯 receive (hdc1080.c:60-72)。
 *   ② BQ27220: 电压≈0mV 判 FAIL (电池是必需件, 不是"台上没插电池")。
 */
#include "hw_devices.h"

#include <math.h>
#include <string.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hw_expect.h"
#include "hw_i2c.h"
#include "hw_report.h"

static const char *TAG = "hwtest";

/* ══════════════════════════════════════════════════════════════════════
 * 通用读写 (每次开临时设备句柄, 用完即删 — 免得缓存句柄把状态带进下一段)
 * ══════════════════════════════════════════════════════════════════════ */
static i2c_master_dev_handle_t open_at(uint8_t addr, int khz)
{
    i2c_master_bus_handle_t bus = hw_i2c_bus();
    if (!bus) return NULL;
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = (uint32_t)khz * 1000,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(bus, &dc, &dev) != ESP_OK) return NULL;
    return dev;
}

static void close_at(i2c_master_dev_handle_t dev)
{
    if (dev) i2c_master_bus_rm_device(dev);
}

/* **真**探测器件在不在: i2c_master_bus_add_device 只分配句柄、**不碰总线**
 * (i2c_master.c:1175-1205 除 OOM 外永远成功) → 拿"句柄非空"当"器件应答"是假象,
 * 判 ACK 必须走 i2c_master_probe。 */
static bool probe_at(uint8_t addr)
{
    i2c_master_bus_handle_t bus = hw_i2c_bus();
    return bus && i2c_master_probe(bus, addr, 100) == ESP_OK;
}

/* 读寄存器: 标准 transmit_receive (多数器件适用) */
static esp_err_t rd_rr(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(dev, &reg, 1, buf, n, 100);
}

/* 写寄存器 */
static esp_err_t wr_r(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(dev, b, 2, 100);
}

/* HDC1080 专用读法: 写指针 → 延时 → 纯 receive (transmit_receive 会 100% 失败) */
static esp_err_t rd_ptr_then_recv(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t n,
                                  int conv_ms)
{
    esp_err_t e = i2c_master_transmit(dev, &reg, 1, 100);
    if (e != ESP_OK) return e;
    vTaskDelay(pdMS_TO_TICKS(conv_ms));
    return i2c_master_receive(dev, buf, n, 100);
}

/* ══════════════════════════════════════════════════════════════════════
 * ES8311 (0x18): 无固定 ID → 身份 = ID 三连读稳定 + 寄存器写-回读一致
 * ══════════════════════════════════════════════════════════════════════ */
static bool es_readback(const char *tag, int *n_ok, char *bad, size_t bad_len)
{
    i2c_master_dev_handle_t dev = open_at(ES8311_I2C_ADDR, 400);
    if (!dev) {
        ESP_LOGE(TAG, "%s: 加设备失败", tag);
        return false;
    }
    int ok = 0;
    size_t bu = 0;
    if (bad_len) bad[0] = '\0';
    for (int i = 0; i < HW_EXP_ES_CMP_N; i++) {
        uint8_t v = 0;
        esp_err_t e = rd_rr(dev, HW_EXP_ES_CMP[i].reg, &v, 1);
        if (e == ESP_OK && v == HW_EXP_ES_CMP[i].val) {
            ok++;
            continue;
        }
        if (bu + 32 < bad_len) {
            if (e == ESP_OK)
                bu += (size_t)snprintf(bad + bu, bad_len - bu, "%s0x%02X=%02X≠%02X", bu ? " " : "",
                                       HW_EXP_ES_CMP[i].reg, v, HW_EXP_ES_CMP[i].val);
            else
                bu += (size_t)snprintf(bad + bu, bad_len - bu, "%s0x%02X 读失败",
                                       bu ? " " : "", HW_EXP_ES_CMP[i].reg);
        }
    }
    close_at(dev);
    if (n_ok) *n_ok = ok;
    return ok == HW_EXP_ES_CMP_N;
}

static void dev_es8311(void)
{
    /* ① 身份: 0xFD/0xFE/0xFF (CHIP ID1/ID2/VERSION) 各读两轮 */
    hw_item_t *it = hw_begin("dev.es8311.id", "ES8311 身份");
    if (!probe_at(ES8311_I2C_ADDR)) {
        hw_set(it, "0x%02X NACK", ES8311_I2C_ADDR);
        hw_note(it, "无 ACK → 器件没在总线上 (未焊/供电缺/脚虚焊)");
        hw_end(it, HW_ST_FAIL);
        return;
    }
    i2c_master_dev_handle_t dev = open_at(ES8311_I2C_ADDR, 400);
    if (!dev) {
        hw_note(it, "ACK 到了但加设备句柄失败");
        hw_end(it, HW_ST_FAIL);
    } else {
        uint8_t r1[3] = {0}, r2[3] = {0};
        bool ok = true;
        for (int k = 0; k < 2; k++) {
            uint8_t *dst = k ? r2 : r1;
            for (int i = 0; i < 3; i++) {
                if (rd_rr(dev, (uint8_t)(HW_EXP_ES_ID_REGS + i), &dst[i], 1) != ESP_OK) ok = false;
            }
        }
        close_at(dev);
        hw_set(it, "ID1=%02X ID2=%02X VER=%02X", r1[0], r1[1], r1[2]);
        bool all_zero = (r1[0] | r1[1] | r1[2]) == 0;
        bool all_ff = (r1[0] & r1[1] & r1[2]) == 0xFF;
        if (!ok) {
            hw_note(it, "ID 寄存器读失败 (NACK/超时)");
            hw_end(it, HW_ST_FAIL);
        } else if (memcmp(r1, r2, 3) != 0) {
            hw_note(it, "两轮读值不一致 (%02X%02X%02X vs %02X%02X%02X) → 数字核心不稳", r1[0], r1[1],
                    r1[2], r2[0], r2[1], r2[2]);
            hw_end(it, HW_ST_FAIL);
        } else if (all_zero || all_ff) {
            hw_note(it, "三个 ID 寄存器全 %s → 应答者是空壳/总线被钳", all_zero ? "0" : "FF");
            hw_end(it, HW_ST_FAIL);
        } else {
            hw_end(it, HW_ST_PASS);
        }
    }

    /* ② 配置: 照 codec 路径顺序写入 → 逐条回读 */
    it = hw_begin("dev.es8311.cfg", "ES8311 配置回读");
    dev = open_at(ES8311_I2C_ADDR, 400);
    if (!dev) {
        hw_note(it, "加设备失败");
        hw_end(it, HW_ST_FAIL);
        return;
    }
    int wrote = 0;
    bool wr_fail = false;
    const char *first_fail = NULL;
    for (int i = 0; i < HW_EXP_ES_SEQ_N; i++) {
        if (wr_r(dev, HW_EXP_ES_SEQ[i].reg, HW_EXP_ES_SEQ[i].val) != ESP_OK) {
            if (!first_fail) first_fail = HW_EXP_ES_SEQ[i].name;
            wr_fail = true;
        } else {
            wrote++;
        }
        esp_rom_delay_us(2000);
    }
    close_at(dev);

    int ok = 0;
    char bad[160];
    bool all_ok = es_readback("ES8311", &ok, bad, sizeof(bad));
    hw_set(it, "写入 %d/%d, 回读 %d/%d", wrote, HW_EXP_ES_SEQ_N, ok, HW_EXP_ES_CMP_N);
    if (wr_fail) {
        hw_note(it, "写入失败于 %s", first_fail ? first_fail : "?");
        hw_end(it, HW_ST_FAIL);
    } else if (!all_ok) {
        hw_note(it, "%s", bad);
        hw_end(it, HW_ST_FAIL);
    } else {
        hw_end(it, HW_ST_PASS);
    }
}

bool hw_devices_es_verify(const char *tag)
{
    int ok = 0;
    char bad[160];
    bool all = es_readback(tag, &ok, bad, sizeof(bad));
    if (!all) ESP_LOGW(TAG, "%s: 回读 %d/%d 不符: %s", tag, ok, HW_EXP_ES_CMP_N, bad);
    return all;
}

/* ══════════════════════════════════════════════════════════════════════
 * HDC1080 (0x40): ID 0x1050 + 配置 14bit
 * ══════════════════════════════════════════════════════════════════════ */
static void dev_hdc1080(void)
{
    hw_item_t *it = hw_begin("dev.hdc1080", "HDC1080 温湿度");
    if (!probe_at(HDC1080_I2C_ADDR)) {
        hw_set(it, "0x%02X NACK", HDC1080_I2C_ADDR);
        hw_note(it, "无 ACK → 器件没在总线上 (未焊/供电缺/脚虚焊)");
        hw_end(it, HW_ST_FAIL);
        return;
    }
    i2c_master_dev_handle_t dev = open_at(HDC1080_I2C_ADDR, 400);
    if (!dev) {
        hw_note(it, "ACK 到了但加设备句柄失败");
        hw_end(it, HW_ST_FAIL);
        return;
    }
    uint8_t id[2] = {0};
    esp_err_t e = rd_ptr_then_recv(dev, 0xFF, id, 2, 10); /* 设备 ID 寄存器 */
    unsigned devid = (unsigned)((id[0] << 8) | id[1]);
    hw_set(it, "ID=0x%04X", devid);
    if (e != ESP_OK) {
        hw_note(it, "读 ID 失败: %s", esp_err_to_name(e));
        close_at(dev);
        hw_end(it, HW_ST_FAIL);
        return;
    }
    if (devid != HW_EXP_HDC_ID) {
        hw_note(it, "期望 0x%04X", HW_EXP_HDC_ID);
        close_at(dev);
        hw_end(it, HW_ST_FAIL);
        return;
    }

    /* 配置: 温湿度都测 + 14bit (hdc1080.c:48) → 回读 top nibble 必须为 0 (无复位/加热) */
    uint8_t cfg[3] = {0x02, 0x00, 0x00};
    e = i2c_master_transmit(dev, cfg, 3, 100);
    if (e != ESP_OK) {
        hw_note(it, "配置写失败: %s", esp_err_to_name(e));
        close_at(dev);
        hw_end(it, HW_ST_FAIL);
        return;
    }
    uint8_t c1[2] = {0}, c2[2] = {0};
    esp_err_t e1 = rd_ptr_then_recv(dev, 0x02, c1, 2, 15);
    esp_err_t e2 = rd_ptr_then_recv(dev, 0x02, c2, 2, 15);
    unsigned cfgv = (unsigned)((c1[0] << 8) | c1[1]);
    hw_set(it, "配置=0x%04X", cfgv);
    if (e1 != ESP_OK || e2 != ESP_OK) {
        hw_note(it, "配置回读失败");
        hw_end(it, HW_ST_FAIL);
    } else if (memcmp(c1, c2, 2) != 0) {
        hw_note(it, "两次回读不一致 (0x%04X/0x%04X)", cfgv, (unsigned)((c2[0] << 8) | c2[1]));
        hw_end(it, HW_ST_FAIL);
    } else if ((cfgv >> 12) != 0) {
        hw_note(it, "高 4 位非 0 → 复位/加热位没清掉");
        hw_end(it, HW_ST_FAIL);
    } else {
        hw_end(it, HW_ST_PASS);
    }
    close_at(dev);
}

/* ══════════════════════════════════════════════════════════════════════
 * OPT3001 (0x44): ID 0x3001 / 厂商 0x5449 + 连续测量配置
 * ══════════════════════════════════════════════════════════════════════ */
static void dev_opt3001(void)
{
    hw_item_t *it = hw_begin("dev.opt3001", "OPT3001 环境光");
    if (!probe_at(OPT3001_I2C_ADDR)) {
        hw_set(it, "0x%02X NACK", OPT3001_I2C_ADDR);
        hw_note(it, "无 ACK → 器件没在总线上 (未焊/供电缺/脚虚焊)");
        hw_end(it, HW_ST_FAIL);
        return;
    }
    i2c_master_dev_handle_t dev = open_at(OPT3001_I2C_ADDR, 400);
    if (!dev) {
        hw_note(it, "ACK 到了但加设备句柄失败");
        hw_end(it, HW_ST_FAIL);
        return;
    }
    uint8_t id[2] = {0}, mf[2] = {0};
    esp_err_t e1 = rd_rr(dev, 0x7F, id, 2);
    esp_err_t e2 = rd_rr(dev, 0x7E, mf, 2);
    if (e1 != ESP_OK || e2 != ESP_OK) {
        hw_set(it, "ID 读失败");
        hw_note(it, "0x7F=%s 0x7E=%s", esp_err_to_name(e1), esp_err_to_name(e2));
        close_at(dev);
        hw_end(it, HW_ST_FAIL);
        return;
    }
    unsigned devid = (unsigned)((id[0] << 8) | id[1]);
    unsigned manuf = (unsigned)((mf[0] << 8) | mf[1]);
    hw_set(it, "ID=0x%04X 厂商=0x%04X", devid, manuf);
    if (devid != HW_EXP_OPT_ID || manuf != HW_EXP_OPT_MANUF) {
        hw_note(it, "期望 ID=0x%04X 厂商=0x%04X", HW_EXP_OPT_ID, HW_EXP_OPT_MANUF);
        close_at(dev);
        hw_end(it, HW_ST_FAIL);
        return;
    }

    /* 配置: 照抄主工程 opt3001.c:51 的字节 (连续测量 800ms + 自动量程) */
    uint8_t cfg[3] = {0x01, (uint8_t)(HW_EXP_OPT_CFG_WRITE >> 8),
                      (uint8_t)(HW_EXP_OPT_CFG_WRITE & 0xFF)};
    esp_err_t e = i2c_master_transmit(dev, cfg, 3, 100);
    if (e != ESP_OK) {
        hw_note(it, "配置写失败: %s", esp_err_to_name(e));
        close_at(dev);
        hw_end(it, HW_ST_FAIL);
        return;
    }
    uint8_t rb[2] = {0};
    esp_err_t er = rd_rr(dev, 0x01, rb, 2);
    unsigned cfgv = (unsigned)((rb[0] << 8) | rb[1]);
    /* 比"写进去的位"是否原样读回; 掩掉 [15:12] RN (自动量程的只读回读, 随环境光变) */
    unsigned got = cfgv & HW_EXP_OPT_CFG_MASK;
    unsigned want = HW_EXP_OPT_CFG_WRITE & HW_EXP_OPT_CFG_MASK;
    hw_set(it, "配置=0x%04X (RN=%u) 期望%u", cfgv, cfgv >> 12, want);
    if (er != ESP_OK) {
        hw_note(it, "配置回读失败");
        hw_end(it, HW_ST_FAIL);
    } else if (got != want) {
        hw_note(it, "配置位回读 0x%03X ≠ 写入 0x%03X (RN 已掩掉)", got, want);
        hw_end(it, HW_ST_FAIL);
    } else {
        /* 首读需 ~800ms — 只记录不判 (暗环境 lux=0 合法) */
        vTaskDelay(pdMS_TO_TICKS(850));
        uint8_t res[2] = {0};
        if (rd_rr(dev, 0x00, res, 2) == ESP_OK) {
            unsigned v = (unsigned)((res[0] << 8) | res[1]);
            unsigned lux10 = (unsigned)((v & 0x0FFF) * (1u << ((v >> 12) & 0x0F))) * 10 / 1000;
            hw_set(it, "照度≈%u.%01u lux", lux10 / 10, lux10 % 10);
        }
        hw_end(it, HW_ST_PASS);
    }
    close_at(dev);
}

/* ══════════════════════════════════════════════════════════════════════
 * BQ27220 (0x55): DeviceType + 电池读数 (电压≈0 → FAIL)
 * ══════════════════════════════════════════════════════════════════════ */
static esp_err_t bq_word(i2c_master_dev_handle_t dev, uint8_t cmd, uint16_t *out)
{
    uint8_t b[2] = {0};
    esp_err_t e = rd_rr(dev, cmd, b, 2);
    if (e == ESP_OK) *out = (uint16_t)(b[0] | (b[1] << 8)); /* 小端 (bq27220.c:33) */
    return e;
}

static void dev_bq27220(void)
{
    hw_item_t *it = hw_begin("dev.bq", "BQ27220 电量计身份");
    if (!probe_at(BQ27220_I2C_ADDR)) {
        hw_set(it, "0x%02X NACK", BQ27220_I2C_ADDR);
        hw_note(it, "无 ACK → 器件没在总线上 (板上唯一 BGA — 优先怀疑焊接)");
        hw_end(it, HW_ST_FAIL);
        hw_bad("dev.bat", "电池读数", "0x55 无 ACK");
        return;
    }
    i2c_master_dev_handle_t dev = open_at(BQ27220_I2C_ADDR, 400);
    if (!dev) {
        hw_note(it, "ACK 到了但加设备句柄失败");
        hw_end(it, HW_ST_FAIL);
        hw_bad("dev.bat", "电池读数", "加设备句柄失败");
        return;
    }

    /* DeviceType: TI 的 Control(0x0001) 读法 —— 本硬件上读回 0x0000, 换个写法又是别的值
     * → 是读法本身不可靠, 不是器件事实:
     * 只留一次单字节读作记录, **不作判据** (身份改由下面"数值是否合理"判)。 */
    uint16_t dt = 0;
    bq_word(dev, 0x01, &dt);

    /* ── 标准命令读数 (主工程 bq27220.c:63-73 用的同几个 cmd) ── */
    uint16_t volt = 0, soc = 0, soh = 0, tmp = 0, cur = 0;
    bool ok = true;
    ok &= (bq_word(dev, 0x08, &volt) == ESP_OK);
    ok &= (bq_word(dev, 0x2C, &soc) == ESP_OK);
    ok &= (bq_word(dev, 0x2E, &soh) == ESP_OK);
    ok &= (bq_word(dev, 0x06, &tmp) == ESP_OK);
    ok &= (bq_word(dev, 0x0C, &cur) == ESP_OK);
    close_at(dev);
    if (!ok) {
        hw_set(it, "标准命令读失败");
        hw_note(it, "0x55 应答但读数不通 → 总线/器件半死");
        hw_end(it, HW_ST_FAIL);
        hw_bad("dev.bat", "电池读数", "读数不通");
        return;
    }

    /* 身份判据 = 数值是否合理 (温度/电压/SOC/SOH 都在物理区间内)。
     * 半死的 0x55 应答者会给出全 0 或越界值 → 光看 ACK 不够。 */
    bool temp_ok = (tmp >= HW_EXP_BAT_TEMP_MIN_K && tmp <= HW_EXP_BAT_TEMP_MAX_K);
    bool volt_ok = (volt <= HW_EXP_BAT_VOLT_MAX_MV);
    bool soc_ok = (soc <= 100 && soh <= 100 && soh > 0);
    hw_set(it, "温度=%.1fC 电压=%umV SOC=%u%% SOH=%u%% | DeviceType=%u (不作判据)",
           (float)tmp / 10.0f - 273.15f, volt, soc, soh, dt);
    if (!temp_ok || !volt_ok || !soc_ok) {
        hw_note(it, "数值不合理 (温度%u 需 %u~%u, 电压%u, SOC%u SOH%u) → 应答者不是活电量计",
                tmp, HW_EXP_BAT_TEMP_MIN_K, HW_EXP_BAT_TEMP_MAX_K, volt, soc, soh);
        hw_end(it, HW_ST_FAIL);
        hw_bad("dev.bat", "电池读数", "身份未确认");
        return;
    }
    hw_end(it, HW_ST_PASS);

    /* ── 电池读数 ── */
    it = hw_begin("dev.bat", "电池读数");
    hw_set(it, "电压=%umV SOC=%u%% SOH=%u%% 电流=%dmA 温度=%.1fC", volt, soc, soh, (int16_t)cur,
           (float)tmp / 10.0f - 273.15f);
    if (volt < HW_EXP_BAT_NOPACK_MV) {
        /* 电池是必需件 → 读不到电芯电压就是故障 (BAT/SRN 没接上) */
        hw_note(it, "电压≈0 (%umV < %umV) → BAT/SRN 没接上", volt, HW_EXP_BAT_NOPACK_MV);
        hw_end(it, HW_ST_FAIL);
    } else if (volt < HW_EXP_BAT_VOLT_MIN_MV || volt > HW_EXP_BAT_VOLT_MAX_MV) {
        hw_note(it, "电压超出 %u~%umV", HW_EXP_BAT_VOLT_MIN_MV, HW_EXP_BAT_VOLT_MAX_MV);
        hw_end(it, HW_ST_FAIL);
    } else if (soc > 100 || soh > 100) {
        hw_note(it, "SOC/SOH 越界 (SOC=%u SOH=%u)", soc, soh);
        hw_end(it, HW_ST_FAIL);
    } else {
        hw_end(it, HW_ST_PASS);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * MPU6500 (0x68): WHO_AM_I + 配置回读 (写入值 = mpu6500.c:87-104)
 * ══════════════════════════════════════════════════════════════════════ */
static void dev_mpu6500(void)
{
    hw_item_t *it = hw_begin("dev.mpu6500", "MPU6500 IMU");
    if (!probe_at(MPU6500_I2C_ADDR)) {
        hw_set(it, "0x%02X NACK", MPU6500_I2C_ADDR);
        hw_note(it, "无 ACK → 器件没在总线上 (未焊/供电缺/脚虚焊)");
        hw_end(it, HW_ST_FAIL);
        return;
    }
    i2c_master_dev_handle_t dev = open_at(MPU6500_I2C_ADDR, 400);
    if (!dev) {
        hw_note(it, "ACK 到了但加设备句柄失败");
        hw_end(it, HW_ST_FAIL);
        return;
    }
    uint8_t who = 0;
    esp_err_t e = rd_rr(dev, 0x75, &who, 1);
    if (e != ESP_OK) {
        hw_note(it, "WHO_AM_I 读失败");
        hw_end(it, HW_ST_FAIL);
        close_at(dev);
        return;
    }
    hw_set(it, "WHO_AM_I=0x%02X", who);
    /* MPU6500 家族 0x70/0x71/0x73/0x74 都算对 (i2c_diag.c:445) */
    if (who != 0x70 && who != 0x71 && who != 0x73 && who != 0x74) {
        hw_note(it, "期望 0x%02X (家族 0x70/71/73/74)", HW_EXP_MPU_WHO);
        hw_end(it, HW_ST_FAIL);
        close_at(dev);
        return;
    }

    /* 软复位 → 唤醒 (mpu6500.c:78-85) */
    wr_r(dev, 0x6B, 0x80);
    vTaskDelay(pdMS_TO_TICKS(100));
    uint8_t pm = 0x80;
    for (int i = 0; i < 20; i++) {
        if (rd_rr(dev, 0x6B, &pm, 1) != ESP_OK) break;
        if (!(pm & 0x80)) break;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    /* 配置写入 */
    for (int i = 0; i < HW_EXP_MPU_REGS_N; i++)
        wr_r(dev, HW_EXP_MPU_REGS[i].reg, HW_EXP_MPU_REGS[i].val);
    esp_rom_delay_us(5000);

    /* 回读比对 */
    int ok = 0;
    char bad[120];
    size_t bu = 0;
    bad[0] = '\0';
    for (int i = 0; i < HW_EXP_MPU_REGS_N; i++) {
        uint8_t v = 0;
        if (rd_rr(dev, HW_EXP_MPU_REGS[i].reg, &v, 1) == ESP_OK && v == HW_EXP_MPU_REGS[i].val) {
            ok++;
        } else if (bu + 24 < sizeof(bad)) {
            bu += (size_t)snprintf(bad + bu, sizeof(bad) - bu, "%s%s=%02X≠%02X", bu ? " " : "",
                                   HW_EXP_MPU_REGS[i].name, v, HW_EXP_MPU_REGS[i].val);
        }
    }
    close_at(dev);
    hw_set(it, "配置回读 %d/%d", ok, HW_EXP_MPU_REGS_N);
    if (ok != HW_EXP_MPU_REGS_N) {
        hw_note(it, "%s", bad);
        hw_end(it, HW_ST_FAIL);
    } else {
        hw_end(it, HW_ST_PASS);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * MPU6500 加速度原始读数 (震动) —— dev.mpu6500 只验配置寄存器能回读,
 * 从没读过一帧加速度: 器件半死/量程错/走线虚焊都能蒙过去。这里补上读数。
 *
 * 量程由 ACCEL_CONFIG(0x1C)[4:3] **现场决定**, 不写死 —— dev.mpu6500 写的是
 * 0x08 (±4g, 8192 LSB/g), 但 app 启动后 DMP 会把它改成 ±2g/16384
 * (inv_mpu.c:633 mpu_set_accel_fsr(2)); 本工程不跑 DMP, 但两套固件都会烧到这块板上,
 * 照寄存器实际值算才不会被上一靴的残留带偏。
 *
 * 判定 = "静止时 |a| 就是重力 1g" —— 物理上说得通才算读数可信。
 * ══════════════════════════════════════════════════════════════════════ */
static float accel_lsb_per_g(uint8_t cfg)
{
    switch ((cfg >> 3) & 0x03) {
    case 0: return 16384.0f; /* ±2g */
    case 1: return 8192.0f;  /* ±4g ← dev.mpu6500 写的 0x08 */
    case 2: return 4096.0f;  /* ±8g */
    default: return 2048.0f; /* ±16g */
    }
}

static int16_t be16(const uint8_t *p)
{
    return (int16_t)((uint16_t)(p[0] << 8) | p[1]);
}

static void dev_accel(void)
{
    hw_item_t *it = hw_begin("dev.accel", "加速度读数 (震动)");
    if (!probe_at(MPU6500_I2C_ADDR)) {
        hw_set(it, "0x%02X NACK", MPU6500_I2C_ADDR);
        hw_note(it, "0x68 不在总线上 → 读不到加速度");
        hw_end(it, HW_ST_FAIL);
        return;
    }
    i2c_master_dev_handle_t dev = open_at(MPU6500_I2C_ADDR, 400);
    uint8_t cfg = 0;
    if (!dev || rd_rr(dev, 0x1C, &cfg, 1) != ESP_OK) {
        if (dev) close_at(dev);
        hw_note(it, "ACCEL_CONFIG(0x1C) 读失败");
        hw_end(it, HW_ST_FAIL);
        return;
    }
    float lsb = accel_lsb_per_g(cfg);

    /* 500ms 窗口: CONFIG(0x1A)=0x04 → DLPF 21Hz, SMPLRT_DIV(0x19)=0x04 → 5ms 出新样本,
     * 所以 10ms 一笔彼此独立 */
    float buf[HW_EXP_ACCEL_SAMPLES][3];
    int n = 0, rd_fail = 0, same = 1;
    uint8_t prev[6] = {0};
    float sx = 0, sy = 0, sz = 0;
    for (int s = 0; s < HW_EXP_ACCEL_SAMPLES; s++) {
        uint8_t r[6];
        if (rd_rr(dev, 0x3B, r, 6) == ESP_OK) { /* ACCEL_XOUT_H .. ZOUT_L */
            if (n && memcmp(r, prev, 6) != 0) same = 0;
            memcpy(prev, r, 6);
            buf[n][0] = (float)be16(r + 0) / lsb;
            buf[n][1] = (float)be16(r + 2) / lsb;
            buf[n][2] = (float)be16(r + 4) / lsb;
            sx += buf[n][0];
            sy += buf[n][1];
            sz += buf[n][2];
            n++;
        } else {
            rd_fail++;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    close_at(dev);
    if (n < HW_EXP_ACCEL_SAMPLES / 2) {
        hw_set(it, "只读到 %d/%d 笔 (读失败 %d)", n, HW_EXP_ACCEL_SAMPLES, rd_fail);
        hw_note(it, "加速度寄存器反复读失败 → 器件半死/走线");
        hw_end(it, HW_ST_FAIL);
        return;
    }

    float mx = sx / (float)n, my = sy / (float)n, mz = sz / (float)n;
    float mag = sqrtf(mx * mx + my * my + mz * mz);
    /* 去重力后的峰值 = "震动幅度": 静止时只剩本底噪声, 手晃板子立刻抬起来
     * (主工程 shake_detector.c:17 的起振门限是 0.18g, 可作对照) */
    float peak = 0;
    for (int i = 0; i < n; i++) {
        float dx = buf[i][0] - mx, dy = buf[i][1] - my, dz = buf[i][2] - mz;
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d > peak) peak = d;
    }
    hw_set(it, "x=%+.2f y=%+.2f z=%+.2f |a|=%.2f g, 峰值抖动 %.3f g (%d 笔, %.0f LSB/g)",
           mx, my, mz, mag, peak, n, lsb);

    if (n > 1 && same) {
        hw_note(it, "%d 笔原始值逐字节相同 → 读数冻结 (传感器没在更新)", n);
        hw_end(it, HW_ST_FAIL);
    } else if (mag < HW_EXP_ACCEL_MIN_G || mag > HW_EXP_ACCEL_MAX_G) {
        hw_note(it, "|a|=%.2f g 偏离重力 1g (期望 %.2f~%.2f) → 量程/器件不对", mag,
                HW_EXP_ACCEL_MIN_G, HW_EXP_ACCEL_MAX_G);
        hw_end(it, HW_ST_FAIL);
    } else {
        hw_end(it, HW_ST_PASS);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * 马达回环 —— dev.accel 只证明"传感器在读数", haptic.* 只验到 EN 脚和 LEDC 通道,
 * 中间那段"电真通到马达、马达真震了"没人管。这里通电短震一下, 看加速度计认不认账。
 *
 * 采一窗加速度, 返回去均值后的峰抖动 (g) 与 |a| 均值。1ms 一笔: pdMS_TO_TICKS(1)
 * 在 100Hz tick 下是 0, 只能忙等。
 * ══════════════════════════════════════════════════════════════════════ */
static void buzz_window(i2c_master_dev_handle_t dev, float lsb, float *peak, float *mag, int *got)
{
    static float buf[HW_EXP_BUZZ_SAMPLES][3]; /* 2.4KB, 放静态不占任务栈 */
    float sx = 0, sy = 0, sz = 0;
    int n = 0;
    for (int s = 0; s < HW_EXP_BUZZ_SAMPLES; s++) {
        uint8_t r[6];
        if (rd_rr(dev, 0x3B, r, 6) == ESP_OK) { /* ACCEL_XOUT_H .. ZOUT_L */
            buf[n][0] = (float)be16(r + 0) / lsb;
            buf[n][1] = (float)be16(r + 2) / lsb;
            buf[n][2] = (float)be16(r + 4) / lsb;
            sx += buf[n][0];
            sy += buf[n][1];
            sz += buf[n][2];
            n++;
        }
        esp_rom_delay_us(1000);
    }
    *got = n;
    if (n == 0) {
        *peak = *mag = 0;
        return;
    }
    float mx = sx / n, my = sy / n, mz = sz / n;
    *mag = sqrtf(mx * mx + my * my + mz * mz);
    float p = 0;
    for (int i = 0; i < n; i++) {
        float dx = buf[i][0] - mx, dy = buf[i][1] - my, dz = buf[i][2] - mz;
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d > p) p = d;
    }
    *peak = p;
}

static void dev_accel_buzz(void)
{
    hw_item_t *it = hw_begin("dev.accel.buzz", "马达回环 (通电短震)");
    if (!probe_at(MPU6500_I2C_ADDR)) {
        hw_set(it, "0x%02X NACK", MPU6500_I2C_ADDR);
        hw_end(it, HW_ST_FAIL);
        return;
    }
    i2c_master_dev_handle_t dev = open_at(MPU6500_I2C_ADDR, 400);
    uint8_t cfg = 0, c1a = 0, c19 = 0;
    if (!dev || rd_rr(dev, 0x1C, &cfg, 1) != ESP_OK || rd_rr(dev, 0x1A, &c1a, 1) != ESP_OK ||
        rd_rr(dev, 0x19, &c19, 1) != ESP_OK) {
        if (dev) close_at(dev);
        hw_note(it, "加速度配置寄存器读失败");
        hw_end(it, HW_ST_FAIL);
        return;
    }
    float lsb = accel_lsb_per_g(cfg);
    /* 临时开到 260Hz 带宽 + 1kHz 输出, 测完还原: 默认 21Hz 带宽会把线性马达
     * 的谐振滤掉, 200Hz 输出采 200Hz 振动还可能整个混叠成直流 → 假"没震" */
    bool wr_ok = wr_r(dev, 0x1A, 0x00) == ESP_OK && wr_r(dev, 0x19, 0x00) == ESP_OK;
    vTaskDelay(pdMS_TO_TICKS(20)); /* 等新配置过一个输出周期 */
    uint8_t v1a = 0xFF, v19 = 0xFF; /* 读回确认快速模式真进去了 (假阴性都出在这里) */
    rd_rr(dev, 0x1A, &v1a, 1);
    rd_rr(dev, 0x19, &v19, 1);

    /* 马达先归零: EN 低 + duty 0 (INPUT_OUTPUT: 纯 OUTPUT 关掉了输入通路,
     * gpio_get_level() 恒返回 0, 读回就验不了真驱动出去没有) */
    gpio_config_t en = {
        .pin_bit_mask = 1ULL << (int)HAPTIC_EN_IO,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&en);
    gpio_set_level(HAPTIC_EN_IO, 0);
    ledc_timer_config_t t = {
        .speed_mode = HW_LEDC_SPD,
        .duty_resolution = HW_HAPTIC_RES,
        .timer_num = HW_HAPTIC_TIMER,
        .freq_hz = HAPTIC_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_channel_config_t ch = {
        .gpio_num = HAPTIC_PWM_IO,
        .speed_mode = HW_LEDC_SPD,
        .channel = HW_HAPTIC_CH,
        .timer_sel = HW_HAPTIC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .intr_type = LEDC_INTR_DISABLE,
    };
    bool led_ok = ledc_timer_config(&t) == ESP_OK && ledc_channel_config(&ch) == ESP_OK;

    float base_peak = 0, base_mag = 0, buzz_peak = 0, buzz_mag = 0;
    int nb = 0, nz = 0;
    buzz_window(dev, lsb, &base_peak, &base_mag, &nb);

    ledc_set_duty(HW_LEDC_SPD, HW_HAPTIC_CH, (uint32_t)HW_EXP_BUZZ_DUTY_PCT * 1023 / 100);
    ledc_update_duty(HW_LEDC_SPD, HW_HAPTIC_CH);
    gpio_set_level(HAPTIC_EN_IO, 1);
    vTaskDelay(pdMS_TO_TICKS(2)); /* duty 下一周期才锁存, 同 G 段那条 */
    uint32_t got_duty = ledc_get_duty(HW_LEDC_SPD, HW_HAPTIC_CH);
    int en_hi = gpio_get_level(HAPTIC_EN_IO); /* 真驱动出去没有: 悬空读 0 = 脚没接通 */
    buzz_window(dev, lsb, &buzz_peak, &buzz_mag, &nz);

    /* 收尾: duty 先归零再断 EN —— 板子交出去时马达必须是停的 */
    ledc_set_duty(HW_LEDC_SPD, HW_HAPTIC_CH, 0);
    ledc_update_duty(HW_LEDC_SPD, HW_HAPTIC_CH);
    gpio_set_level(HAPTIC_EN_IO, 0);
    bool rb_ok = wr_r(dev, 0x1A, c1a) == ESP_OK && wr_r(dev, 0x19, c19) == ESP_OK;
    close_at(dev);

    hw_set(it, "静止 峰%.3f → 震中 峰%.3f g (|a| %.2f→%.2f) | %d%% duty %d 笔, EN 读回 %d, duty 读回 %u",
           base_peak, buzz_peak, base_mag, buzz_mag, HW_EXP_BUZZ_DUTY_PCT, nz, en_hi,
           (unsigned)got_duty);
    if (!led_ok || !wr_ok) {
        hw_note(it, "LEDC/IMU 配置写入失败 → 本项无效");
        hw_end(it, HW_ST_FAIL);
        return;
    }
    if (v1a != 0x00 || v19 != 0x00) { /* 快速模式没进去 → 这一项的结论不可信 */
        hw_note(it, "IMU 快速采样未生效 (0x1A=0x%02X 0x19=0x%02X, 期望 0x00) → 本项无效", v1a, v19);
        hw_end(it, HW_ST_FAIL);
        return;
    }
    if (nb < HW_EXP_BUZZ_SAMPLES / 2 || nz < HW_EXP_BUZZ_SAMPLES / 2) {
        hw_note(it, "读数笔数不足 (读失败太多)");
        hw_end(it, HW_ST_FAIL);
        return;
    }
    if (!rb_ok) hw_note(it, "IMU 采样配置未还原 (0x1A/0x19)");
    if (base_peak > HW_EXP_BUZZ_BUSY_G) {
        /* 静止窗都不安静 → 基线不可用, 这时候任何比值都不作数 */
        hw_note(it, "静止窗峰抖动 %.3f g > %.3f g → 板子当时在被搬动, 基线不可用, 重跑",
                base_peak, HW_EXP_BUZZ_BUSY_G);
        hw_end(it, HW_ST_WARN);
        return;
    }

    /* 绝对下限兜底: 基线太平 (峰抖动≈0) 时纯比值会放过本底噪声 */
    float need = base_peak * HW_EXP_BUZZ_GAIN_MIN;
    if (need < HW_EXP_BUZZ_MIN_G) need = HW_EXP_BUZZ_MIN_G;

    if (buzz_peak >= need) {
        hw_end(it, HW_ST_PASS);
    } else if (buzz_peak >= need * 0.5f) {
        hw_note(it, "振幅只有 %.3f g (需 %.3f g) → 马达弱/机械卡涩/供电不足", buzz_peak, need);
        hw_end(it, HW_ST_WARN);
    } else if (en_hi != 1) {
        hw_note(it, "EN 驱动高后读回 %d → 脚没被拉起来, 马达根本没通电", en_hi);
        hw_end(it, HW_ST_FAIL);
    } else {
        hw_note(it, "EN/duty 都已到位而加速度计纹丝不动 (%.3f g, 基线 %.3f g) → 马达没震: 焊点/马达本体",
                buzz_peak, base_peak);
        hw_end(it, HW_ST_FAIL);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * QMC6309 地磁: 挂 MPU6500 的 AUX, 靠 INT_PIN_CFG.BYPASS_EN 桥到主总线
 *
 * 贴装因板而异: 贴了 → 旁路打开后 **0x0C 应答** (reg 0x00 = 0x90, reg 0x0D = 0x00),
 *   主工程那个 0x2C 从不应答 (那一处驱动从没被调用过); 没贴 → 0x0C 与 0x2C 都 NACK。
 * → 两个地址都 NACK 判 FAIL, 0x0C 应答才去读 ID。
 * (曾把"0x2C 有应答"当结论 —— 那是 add_device 不探总线造成的假象。)
 * ══════════════════════════════════════════════════════════════════════ */
static void dev_qmc6309(void)
{
    hw_item_t *it = hw_begin("dev.qmc6309", "QMC6309 地磁(AUX)");
    /* 前置: MPU6500 的 INT_PIN_CFG 必须是 0x02 (BYPASS_EN=1), dev.mpu6500 已写好, 这里再确认一次 */
    if (!probe_at(MPU6500_I2C_ADDR)) {
        hw_note(it, "MPU6500 无 ACK → 先修 0x68, AUX 旁路无从谈起");
        hw_end(it, HW_ST_FAIL);
        return;
    }
    i2c_master_dev_handle_t mpu = open_at(MPU6500_I2C_ADDR, 400);
    if (!mpu) {
        hw_note(it, "加 MPU6500 句柄失败");
        hw_end(it, HW_ST_FAIL);
        return;
    }
    wr_r(mpu, 0x37, 0x02);
    esp_rom_delay_us(2000);
    uint8_t cfg = 0;
    rd_rr(mpu, 0x37, &cfg, 1);
    close_at(mpu);
    if (!(cfg & 0x02)) {
        hw_set(it, "INT_PIN_CFG=0x%02X", cfg);
        hw_note(it, "BYPASS_EN 没置上 → AUX 上的地磁不会出现在主总线");
        hw_end(it, HW_ST_FAIL);
        return;
    }

    bool a_p = probe_at(HW_EXP_QMC_ADDR);     /* 0x0C: 旁路开后应答的地址 */
    bool a_alt = probe_at(HW_EXP_QMC_ADDR_ALT); /* 0x2C: 主工程常量, 本硬件无应答 */
    if (!a_p && !a_alt) {
        hw_set(it, "INT_PIN_CFG=0x%02X(BYPASS_EN ✓), 0x%02X 与 0x%02X 均 NACK", cfg,
               HW_EXP_QMC_ADDR, HW_EXP_QMC_ADDR_ALT);
        /* 检测不到 = 硬件错误 (未贴装 / 虚焊 / 旁路未接通) */
        hw_note(it, "两个地址都 NACK → 总线上找不到 6309");
        hw_end(it, HW_ST_FAIL);
        return;
    }
    uint8_t addr = a_p ? HW_EXP_QMC_ADDR : HW_EXP_QMC_ADDR_ALT;
    i2c_master_dev_handle_t dev = open_at(addr, 400);
    if (!dev) {
        hw_set(it, "ACK 于 0x%02X", addr);
        hw_note(it, "ACK 到了但加设备句柄失败");
        hw_end(it, HW_ST_FAIL);
        return;
    }
    uint8_t idv = 0, wia = 0;
    esp_err_t e1 = rd_rr(dev, HW_EXP_QMC_ID_REG, &idv, 1);
    esp_err_t e2 = rd_rr(dev, HW_EXP_QMC_WIA_REG, &wia, 1);
    close_at(dev);
    hw_set(it, "INT_PIN_CFG=0x%02X, ACK 于 0x%02X", cfg, addr);
    /* 地址 ACK 了却读不出 ID = "半死应答者"特征 (器件半死时就是这样) → FAIL */
    if (e1 != ESP_OK) {
        hw_note(it, "地址 ACK 但 ID(0x%02X) 读失败 → 半死应答者 (器件/走线), 不是没贴装",
                HW_EXP_QMC_ID_REG);
        hw_end(it, HW_ST_FAIL);
        return;
    }
    hw_note(it, "ID(0x%02X)=0x%02X (期望 0x%02X)%s", HW_EXP_QMC_ID_REG, idv, HW_EXP_QMC_ID,
            (e2 == ESP_OK) ? "" : " [0x0D 读失败]");
    if (idv == HW_EXP_QMC_ID) {
        if (e2 == ESP_OK && wia != 0x00)
            hw_note(it, "0x0D=0x%02X (主工程驱动声称 0x31, 本硬件读回 0x00 → 不判定)", wia);
        hw_end(it, HW_ST_PASS);
    } else {
        hw_end(it, HW_ST_WARN);
    }
}

void hw_devices_run(void)
{
    ESP_LOGI(TAG, "──── C 段: 器件识别/通讯/配置 ────");
    dev_es8311();
    dev_hdc1080();
    dev_opt3001();
    dev_bq27220();
    dev_mpu6500();
    dev_accel();
    dev_accel_buzz();
    dev_qmc6309();
}

/* ══════════════════════════════════════════════════════════════════════
 * F 段 · 麦克风读数
 *
 * 麦克风的数据挂在编解码器 ADC 侧, 从 I2S DIN(GPIO16) 回来。采样是**交错立体声**
 * (本段建的是 STEREO, 不是主工程播放用的 MONO) → 两个槽分别统计, 判在响的那个,
 * 因为"哪个槽装麦克风"取决于 codec 侧寄存器, 不写死。
 *
 * 判定只认"整窗 min==max"= ADC 没在采样 (静音/未贴装/半死)。**电平不参与判定**:
 * 房间噪声、人声、空调都能把 RMS 抬起来, 拿绝对电平卡阈值会把环境当故障。
 * ══════════════════════════════════════════════════════════════════════ */
#define MIC_CHUNK_FRAMES 480 /* 10ms 一块: 一块一收, 不撑爆 RX 那 30ms 的 DMA 深度 */
#define MIC_FRAMES (AUDIO_SAMPLE_RATE * HW_EXP_MIC_MS / 1000)
#define MIC_BYTES (MIC_FRAMES * 4) /* 2 槽 × 16bit */
#define TWO_PI 6.283185307179586f  /* 不倚赖 M_PI (newlib 要看特性开关) */

/* 单频检波 (Goertzel): 只算一个频点, 比整段 FFT 便宜得多。
 * 整窗 14400 帧 @48kHz 时 440Hz 正好 132 个周期 —— **整数周期 → 该格无频谱泄漏**,
 * 归一化后的幅度可以直接和采样幅度 (LSB) 比。 */
typedef struct {
    float cw, s1, s2;
} goertzel_t;

static void go_init(goertzel_t *g, float freq, float fs)
{
    g->cw = 2.0f * cosf(TWO_PI * freq / fs);
    g->s1 = 0.0f;
    g->s2 = 0.0f;
}

static void go_step(goertzel_t *g, float x)
{
    float s0 = x + g->cw * g->s1 - g->s2;
    g->s2 = g->s1;
    g->s1 = s0;
}

/* 归一成"振幅 (LSB)": 整数周期窗上纯音的 mag = A·N/2 */
static float go_amp(const goertzel_t *g, int n)
{
    float m2 = g->s1 * g->s1 + g->s2 * g->s2 - g->cw * g->s1 * g->s2;
    return (m2 > 0.0f && n > 0) ? sqrtf(m2) / ((float)n * 0.5f) : 0.0f;
}

typedef struct {
    int frames;
    float rms[2], peak[2], dc[2]; /* 交流 RMS / 峰值 / 直流偏置 (LSB) */
    float tone[2];                /* 单音频点处的振幅 (LSB), 整数周期归一 */
    int frozen[2];                /* 该槽整窗 min==max */
} mic_win_t;

static void mic_analyze(const uint8_t *buf, int frames, mic_win_t *w)
{
    double sum[2] = {0, 0}, sq[2] = {0, 0};
    int mn[2] = {32767, 32767}, mx[2] = {-32768, -32768};
    goertzel_t g[2];
    for (int c = 0; c < 2; c++) go_init(&g[c], HW_EXP_SPK_TONE_HZ, (float)AUDIO_SAMPLE_RATE);
    for (int i = 0; i < frames; i++) {
        for (int c = 0; c < 2; c++) {
            int16_t v = (int16_t)((uint16_t)buf[i * 4 + c * 2] |
                                  ((uint16_t)buf[i * 4 + c * 2 + 1] << 8));
            sum[c] += v;
            sq[c] += (double)v * (double)v;
            if (v < mn[c]) mn[c] = v;
            if (v > mx[c]) mx[c] = v;
            go_step(&g[c], (float)v);
        }
    }
    w->frames = frames;
    for (int c = 0; c < 2; c++) {
        double dc = sum[c] / (double)frames;
        double ac = sq[c] / (double)frames - dc * dc;
        w->dc[c] = (float)dc;
        w->rms[c] = (ac > 0.0) ? (float)sqrt(ac) : 0.0f;
        w->peak[c] = (float)((mx[c] > -mn[c]) ? mx[c] : -mn[c]);
        w->frozen[c] = (mn[c] == mx[c]);
        w->tone[c] = go_amp(&g[c], frames);
    }
}

/* 采一窗 (不播放时用), 返回实际收到的帧数 */
static int mic_fill_quiet(i2s_chan_handle_t rx, uint8_t *buf, int frames)
{
    int got_frames = 0;
    while (got_frames < frames) {
        int want = frames - got_frames;
        if (want > MIC_CHUNK_FRAMES) want = MIC_CHUNK_FRAMES;
        size_t got = 0;
        if (i2s_channel_read(rx, buf + (size_t)got_frames * 4, (size_t)want * 4, &got, 300) !=
            ESP_OK)
            break;
        if (got == 0) break;
        got_frames += (int)(got / 4);
    }
    return got_frames;
}

static void audio_mic(i2s_chan_handle_t rx)
{
    hw_item_t *it = hw_begin("audio.mic", "麦克风读数");
    uint8_t *buf = heap_caps_malloc(MIC_BYTES, MALLOC_CAP_SPIRAM);
    if (!buf) {
        hw_note(it, "采样缓冲分配失败 (PSRAM)");
        hw_end(it, HW_ST_FAIL);
        return;
    }
    int n = mic_fill_quiet(rx, buf, MIC_FRAMES);
    if (n < MIC_FRAMES / 2) {
        hw_set(it, "只收到 %d/%d 帧", n, MIC_FRAMES);
        hw_note(it, "I2S 读不到采样 → 编解码器 ADC 侧没出数");
        hw_end(it, HW_ST_FAIL);
        heap_caps_free(buf);
        return;
    }
    mic_win_t w;
    mic_analyze(buf, n, &w);
    heap_caps_free(buf);
    hw_set(it, "左 RMS %.1f 峰 %.0f DC %+.0f | 右 RMS %.1f 峰 %.0f DC %+.0f (%.0fms)", w.rms[0],
           w.peak[0], w.dc[0], w.rms[1], w.peak[1], w.dc[1],
           (float)n * 1000.0f / (float)AUDIO_SAMPLE_RATE);
    if (w.frozen[0] && w.frozen[1]) {
        hw_note(it, "两个槽整窗恒定 → ADC 没在采样 (麦克风未贴装/静音/半死)");
        hw_end(it, HW_ST_FAIL);
    } else {
        hw_end(it, HW_ST_PASS);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * F 段 · 扬声器回采 —— 本工程**唯一出声**的地方 (300ms 一声), 其余全程静音。
 *
 * 喇叭和麦克风挨着 → "有没有出声"可以用声学回路自动判, 不必等人耳。
 * 判据 = 播放前后**自己比自己** (静音底噪 vs 播放中) 在同一单音频点上的提升倍数,
 * 所以房间本来多吵不影响结论, 也不需要事先标定麦克风灵敏度。
 *
 * ⚠️ DAC 音量/静音 (0x31/0x32) 本段之前从没写过 —— 缺省值不可控, 不写就可能是静音。
 *    音量取 app 播 TTS 时用的那个寄存器值, 验的就是真实播放配置。
 * ══════════════════════════════════════════════════════════════════════ */
static void amp_enable(bool on)
{
    static bool cfg_done = false;
    if (!cfg_done) {
        gpio_config_t pc = {
            .pin_bit_mask = 1ULL << (int)AUDIO_AMP_EN_IO,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE, /* 同 es8311_drv.c:58 */
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&pc);
        cfg_done = true;
    }
    gpio_set_level(AUDIO_AMP_EN_IO, on ? 1 : 0);
}

/* 把 RX DMA 里积压的旧数据读掉: RX 有 30ms 深, 不清的话回采窗口开头
 * 混进播放前的静音, 把提升倍数拉低 */
static void mic_drain(i2s_chan_handle_t rx, uint8_t *buf)
{
    for (int i = 0; i < 4; i++) {
        size_t got = 0;
        if (i2s_channel_read(rx, buf, MIC_CHUNK_FRAMES * 4, &got, 100) != ESP_OK) break;
    }
}

/* DAC 侧寄存器实况 —— 只读, 不判死。
 * 这 12 个是主工程 es8311_drv.c 会写、HW_EXP_ES_SEQ 不写的 (0x09/0x0A 串口格式、
 * 0x0D VREF/VMID 等), 看初始化把片子留在什么状态。
 * ★ 结果走 hw_set 报出 —— 复位后前段串口的日志收不到 (见 run.py)。 */
static void audio_dacreg(void)
{
    hw_item_t *it = hw_begin("audio.dacreg", "ES8311 DAC 侧寄存器");
    i2c_master_dev_handle_t dev = open_at(ES8311_I2C_ADDR, 400);
    if (!dev) {
        hw_set(it, "0x18 打不开");
        hw_end(it, HW_ST_FAIL);
        return;
    }
    uint8_t dr[12];
    int bad = 0;
    static const uint8_t dbg[12] = {0x09, 0x0A, 0x0D, 0x12, 0x13,
                                    0x14, 0x1B, 0x1C, 0x31, 0x32, 0x37, 0x44};
    for (int i = 0; i < 12; i++) {
        if (rd_rr(dev, dbg[i], &dr[i], 1) != ESP_OK) {
            dr[i] = 0;
            bad++;
        }
    }
    close_at(dev);
    hw_set(it, "09=%02X 0A=%02X 0D=%02X 12=%02X 13=%02X 14=%02X "
               "1B=%02X 1C=%02X 31=%02X 32=%02X 37=%02X 44=%02X",
           dr[0], dr[1], dr[2], dr[3], dr[4], dr[5], dr[6], dr[7], dr[8], dr[9], dr[10], dr[11]);
    if (bad) {
        hw_note(it, "%d 个寄存器读失败", bad);
        hw_end(it, HW_ST_FAIL);
    } else {
        hw_end(it, HW_ST_PASS);
    }
}

/* 功放使能脚 (TPA2011D1, 高有效) —— 只验 MCU 侧驱动得起这根脚, 不验功放芯片。
 * 读法照 haptic.en: 驱动低/高各读回一次, 再当输入看悬空电位。 */
static void audio_pa(void)
{
    hw_item_t *it = hw_begin("audio.pa", "功放使能脚");
    /* ★ 要读回就得 INPUT_OUTPUT: GPIO_MODE_OUTPUT 关掉了输入通路,
     * gpio_get_level() 恒返回 0 (看着像脚被钉死在低电平)。 */
    gpio_config_t oc = {.pin_bit_mask = 1ULL << (int)AUDIO_AMP_EN_IO,
                        .mode = GPIO_MODE_INPUT_OUTPUT,
                        .pull_up_en = GPIO_PULLUP_DISABLE,
                        .pull_down_en = GPIO_PULLDOWN_ENABLE,
                        .intr_type = GPIO_INTR_DISABLE};
    gpio_config(&oc);
    gpio_set_level(AUDIO_AMP_EN_IO, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    int lo = gpio_get_level(AUDIO_AMP_EN_IO);
    gpio_set_level(AUDIO_AMP_EN_IO, 1);
    vTaskDelay(pdMS_TO_TICKS(2));
    int hi = gpio_get_level(AUDIO_AMP_EN_IO);

    /* 悬空电位: 当输入, 内部上拉/下拉各读一次 —— 分辨线被外部拉死还是 pad 自己的问题 */
    gpio_config_t ic = oc;
    ic.mode = GPIO_MODE_INPUT;
    ic.pull_up_en = GPIO_PULLUP_ENABLE;
    ic.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&ic);
    vTaskDelay(pdMS_TO_TICKS(2));
    int pu = gpio_get_level(AUDIO_AMP_EN_IO);
    ic.pull_up_en = GPIO_PULLUP_DISABLE;
    ic.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&ic);
    vTaskDelay(pdMS_TO_TICKS(2));
    int pd = gpio_get_level(AUDIO_AMP_EN_IO);

    gpio_config(&oc); /* 还原成输出: 后面 audio_spk 还要拉它 */
    gpio_set_level(AUDIO_AMP_EN_IO, 0);

    hw_set(it, "GPIO%d 驱动 低=%d 高=%d | 悬空 上拉=%d 下拉=%d (期望 0/1 | 1/0)",
           (int)AUDIO_AMP_EN_IO, lo, hi, pu, pd);
    /* 只报实测, 不替硬件下结论 */
    if (lo == 0 && hi == 1) {
        hw_end(it, HW_ST_PASS);
    } else if (lo == 1 && hi == 1) {
        hw_note(it, "驱动低也读回 1 = 线上一直有高电平, 软件控不住这根脚");
        hw_end(it, HW_ST_FAIL);
    } else {
        hw_note(it, "驱动跟读回对不上 (悬空时 上拉=%d 下拉=%d)", pu, pd);
        hw_end(it, HW_ST_FAIL);
    }
}

static void audio_spk(i2s_chan_handle_t tx, i2s_chan_handle_t rx)
{
    hw_item_t *it = hw_begin("audio.spk", "扬声器回采");
    uint8_t *buf = heap_caps_malloc(MIC_BYTES, MALLOC_CAP_SPIRAM);
    if (!buf) {
        hw_note(it, "采样缓冲分配失败 (PSRAM)");
        hw_end(it, HW_ST_FAIL);
        return;
    }

    /* ① 静音底噪 */
    amp_enable(false);
    mic_drain(rx, buf);
    int n0 = mic_fill_quiet(rx, buf, MIC_FRAMES);
    if (n0 < MIC_FRAMES / 2) {
        hw_set(it, "静音段只收到 %d/%d 帧", n0, MIC_FRAMES);
        hw_note(it, "还没播放就收不到采样 → 先修麦克风那一项");
        hw_end(it, HW_ST_FAIL);
        heap_caps_free(buf);
        return;
    }
    mic_win_t q;
    mic_analyze(buf, n0, &q);

    /* ② 写 DAC 音量 + 解静音 (0x31 = 静音, 0x32 = 音量) */
    i2c_master_dev_handle_t dev = open_at(ES8311_I2C_ADDR, 400);
    uint8_t mt = 0;
    bool dac_ok = (dev != NULL);
    if (dev) {
        dac_ok &= (rd_rr(dev, 0x31, &mt, 1) == ESP_OK);
        dac_ok &= (wr_r(dev, 0x31, (uint8_t)(mt & 0x9F)) == ESP_OK); /* 清 bit5/bit6 */
        dac_ok &= (wr_r(dev, 0x32, HW_EXP_SPK_VOL_REG) == ESP_OK);
        close_at(dev);
    }

    /* ③ 边推边收 —— RX 的 DMA 只有 30ms 深, 不边收必然溢出丢数据 */
    amp_enable(true);
    vTaskDelay(pdMS_TO_TICKS(30)); /* 等功放上电稳住 */
    const float step = TWO_PI * HW_EXP_SPK_TONE_HZ / (float)AUDIO_SAMPLE_RATE;
    float phase = 0.0f;
    int n1 = 0, chunks = MIC_FRAMES / MIC_CHUNK_FRAMES;
    bool wr_ok = true;
    size_t pushed = 0;
    for (int c = 0; c < chunks; c++) {
        int16_t pcm[MIC_CHUNK_FRAMES * 2];
        for (int i = 0; i < MIC_CHUNK_FRAMES; i++) {
            int idx = c * MIC_CHUNK_FRAMES + i;
            /* 头 20ms 淡入: 直接起音会在功放上打出一个爆音 */
            float env = (idx < 960) ? ((float)idx / 960.0f) : 1.0f;
            int16_t v = (int16_t)(sinf(phase) * (float)HW_EXP_SPK_AMP * env);
            pcm[i * 2] = v;
            pcm[i * 2 + 1] = v; /* 两槽同数据: app 的 MONO 槽模式硬件也是这么复制的 */
            phase += step;
            if (phase > TWO_PI) phase -= TWO_PI;
        }
        size_t w = 0;
        if (i2s_channel_write(tx, pcm, sizeof(pcm), &w, 500) != ESP_OK || w != sizeof(pcm))
            wr_ok = false;
        pushed += w;
        size_t r = 0;
        if (i2s_channel_read(rx, buf + (size_t)n1 * 4, MIC_CHUNK_FRAMES * 4, &r, 500) == ESP_OK)
            n1 += (int)(r / 4);
    }
    amp_enable(false); /* 交出去时功放必须是关的 */

    mic_win_t p;
    mic_analyze(buf, (n1 > 0) ? n1 : 1, &p);
    heap_caps_free(buf);

    int ci = (p.tone[1] > p.tone[0]) ? 1 : 0; /* 判在响的那个槽 */
    float base = q.tone[ci], got = p.tone[ci];
    /* +1 平滑: 底噪可能恰好为 0, 直接相除会爆 */
    float gain = (got + 1.0f) / (base + 1.0f);
    /* 宽带 RMS 一并报: 单频不动而宽带抬 = 在响但不是这个频点; 两个都不动 = 没出声 */
    float rq = (q.rms[0] + q.rms[1]) * 0.5f, rp = (p.rms[0] + p.rms[1]) * 0.5f;
    hw_set(it, "440Hz 底噪 %.1f → 播放中 %.1f (%.1f 倍); 宽带RMS %.1f → %.1f, 推 %u 字节 %s",
           base, got, gain, rq, rp, (unsigned)pushed, dac_ok ? "" : "[DAC 寄存器写失败]");

    if (!dac_ok) {
        hw_note(it, "0x18 的 DAC 音量/静音寄存器写不进去 → 播放通路配置失败");
        hw_end(it, HW_ST_FAIL);
    } else if (!wr_ok) {
        hw_note(it, "I2S TX 没推完 (%u 字节) → 播放通路不通", (unsigned)pushed);
        hw_end(it, HW_ST_FAIL);
    } else if (n1 < MIC_FRAMES / 2) {
        hw_note(it, "回采只收到 %d/%d 帧 → 麦克风没在收", n1, MIC_FRAMES);
        hw_end(it, HW_ST_FAIL);
    } else if (gain < HW_EXP_SPK_LOOP_GAIN_MIN) {
        hw_note(it, "回采无提升 (<%.1f 倍) → 功放/喇叭没响 (或麦克风收不到)",
                HW_EXP_SPK_LOOP_GAIN_MIN);
        hw_end(it, HW_ST_FAIL);
    } else {
        hw_end(it, HW_ST_PASS);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * F 段: 音频通路
 *   ① I2S 通道建立 + 使能 → MCLK 12.288MHz 真的跑起来
 *   ② 麦克风读数 — 判定只认"没在采样", 电平只报不判
 *   ③ DAC 侧寄存器实况 — 只读
 *   ④ 功放使能脚 — 只验 MCU 侧推得动这根脚, 不验功放芯片
 *   ⑤ 扬声器回采 — 放一声 440Hz, 用麦克风验"真出声了"
 *   ⑥ 编解码器寄存器是否仍一致 (半死 codec 在这里最容易露)
 * ══════════════════════════════════════════════════════════════════════ */
void hw_audio_run(void)
{
    ESP_LOGI(TAG, "──── F 段: 音频通路 (无声) ────");
    hw_item_t *it = hw_begin("audio.i2s", "I2S 通道 + 麦克风采样");

    i2s_chan_handle_t tx = NULL, rx = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;
    esp_err_t e = i2s_new_channel(&chan_cfg, &tx, &rx);
    if (e != ESP_OK) {
        hw_note(it, "i2s_new_channel: %s", esp_err_to_name(e));
        hw_end(it, HW_ST_FAIL);
        return;
    }
    /* gpio 表照 es8311_drv.c:78 (mclk=GPIO14 bclk=15 ws=17 dout=18 din=16) */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = AUDIO_MCLK_IO,
            .bclk = AUDIO_DMIC_SCL_IO,
            .ws = AUDIO_LRCK_IO,
            .dout = AUDIO_DSDIN_IO,
            .din = AUDIO_ASDOUT_IO,
        },
    };
    esp_err_t e1 = i2s_channel_init_std_mode(tx, &std_cfg);
    esp_err_t e2 = i2s_channel_init_std_mode(rx, &std_cfg);
    esp_err_t e3 = i2s_channel_enable(tx);
    esp_err_t e4 = i2s_channel_enable(rx);
    bool i2s_up = (e1 == ESP_OK && e2 == ESP_OK && e3 == ESP_OK && e4 == ESP_OK);
    hw_set(it, "%dHz 全双工 init=%s/%s en=%s/%s", AUDIO_SAMPLE_RATE, esp_err_to_name(e1),
           esp_err_to_name(e2), esp_err_to_name(e3), esp_err_to_name(e4));
    if (!i2s_up) {
        hw_note(it, "I2S 通道建立/使能失败");
        hw_end(it, HW_ST_FAIL);
    } else {
        /* 通道起来的粗验: 有数据流回来即证明 MCLK/BCLK/LRCK/DIN 通路在跑。
         * 电平/冻结的细判在 audio.mic (读法太糙, 放这儿会把"没焊麦克风"混进来) */
        size_t want = 4096;
        static uint8_t buf[4096];
        size_t got = 0;
        esp_err_t er = i2s_channel_read(rx, buf, want, &got, 300);
        int nonzero = 0;
        for (size_t i = 0; i < got; i++)
            if (buf[i]) nonzero++;
        if (er == ESP_OK && got > 0) {
            hw_set(it, "收 %u 字节, 非 0 %d", (unsigned)got, nonzero);
            hw_end(it, HW_ST_PASS);
        } else {
            hw_note(it, "I2S 读超时/无数据: %s (got=%u)", esp_err_to_name(er), (unsigned)got);
            hw_end(it, HW_ST_FAIL);
        }
    }

    /* 功放使能脚和 I2S 无关, 先单独验 */
    audio_pa();

    /* ②③④ 麦克风读数 / DAC 侧寄存器实况 / 扬声器回采
     * (通道没起来就别重复报 FAIL, 记 SKIP) */
    if (i2s_up) {
        audio_mic(rx);
        audio_dacreg(); /* 播放之前的状态: audio.spk 会改 0x31/0x32, 得先读 */
        audio_spk(tx, rx);
    } else {
        hw_skip("audio.mic", "麦克风读数", "I2S 通道没起来");
        hw_skip("audio.dacreg", "ES8311 DAC 侧寄存器", "I2S 通道没起来");
        hw_skip("audio.spk", "扬声器回采", "I2S 通道没起来");
    }

    /* ④ 时钟跑着的时候复读编解码器寄存器 */
    hw_item_t *it2 = hw_begin("audio.codec", "MCLK 下的 ES8311 回读");
    bool ok = hw_devices_es_verify("audio.codec");
    hw_set(it2, "回读 %s (MCLK %uHz 在跑)", ok ? "全部一致" : "有偏差", (unsigned)AUDIO_MCLK_HZ);
    hw_end(it2, ok ? HW_ST_PASS : HW_ST_FAIL);
    if (!hw_i2c_lines_free())
        hw_note(it2, "线被钳住 — 有时钟才咬 = 半死 codec 特征");

    /* 收尾: 关通道 + 放开 MCLK 脚 (后面 i2c.mclk 段要用 LEDC 接管它) */
    i2s_channel_disable(tx);
    i2s_channel_disable(rx);
    i2s_del_channel(tx);
    i2s_del_channel(rx);
    gpio_reset_pin(AUDIO_MCLK_IO);
    ESP_LOGI(TAG, "I2S 已拆, MCLK 脚已放开");
}
