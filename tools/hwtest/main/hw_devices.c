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
 *   ② BQ27220: 无电池时电压≈0mV 是测试台常态 ⇒ 不判 FAIL 只 SKIP。
 */
#include "hw_devices.h"

#include <string.h>

#include "board.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
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
 * BQ27220 (0x55): DeviceType + 电池读数 (无电池 → SKIP)
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
        hw_skip("dev.bat", "电池读数", "读数不通");
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
        hw_skip("dev.bat", "电池读数", "身份未确认");
        return;
    }
    hw_end(it, HW_ST_PASS);

    /* ── 电池读数 ── */
    it = hw_begin("dev.bat", "电池读数");
    hw_set(it, "电压=%umV SOC=%u%% SOH=%u%% 电流=%dmA 温度=%.1fC", volt, soc, soh, (int16_t)cur,
           (float)tmp / 10.0f - 273.15f);
    if (volt < HW_EXP_BAT_NOPACK_MV) {
        /* 测试台常态: 没插电池; 也可能是 BGA 电压采样脚虚焊 → 先当 SKIP */
        hw_note(it, "电压≈0 → 测试台上没有电池 (或 BAT/SRN 虚焊) — 不判 FAIL");
        hw_end(it, HW_ST_SKIP);
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
 * QMC6309 地磁: 挂 MPU6500 的 AUX, 靠 INT_PIN_CFG.BYPASS_EN 桥到主总线
 *
 * 贴装因板而异: 贴了 → 旁路打开后 **0x0C 应答** (reg 0x00 = 0x90, reg 0x0D = 0x00),
 *   主工程那个 0x2C 从不应答 (那一处驱动从没被调用过); 没贴 → 0x0C 与 0x2C 都 NACK。
 * → 两个地址都 NACK 判 SKIP (本板没装是正常的), 0x0C 应答才去读 ID。
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
        /* 贴装因板而异: 没贴的板两个地址都 NACK, app 也从不调 qmc6309_init
         * → "不在总线上"不算错, 判 SKIP。真焊上了就该走下面的 ID 分支。 */
        hw_note(it, "本板未贴装 6309; 若本板应贴装则人工确认");
        hw_end(it, HW_ST_SKIP);
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
    dev_qmc6309();
}

/* ══════════════════════════════════════════════════════════════════════
 * F 段: 音频通路 (不出声)
 *   ① I2S 通道建立 + 使能 → MCLK 12.288MHz 真的跑起来 (app 6.45s 干的就是这件事)
 *   ② 时钟上电后编解码器寄存器是否仍一致 (半死 codec 在这里最容易露)
 *   ③ 麦克风侧只读"有无采样在流动", 不判音量
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
    hw_set(it, "%dHz 全双工 init=%s/%s en=%s/%s", AUDIO_SAMPLE_RATE, esp_err_to_name(e1),
           esp_err_to_name(e2), esp_err_to_name(e3), esp_err_to_name(e4));
    if (e1 != ESP_OK || e2 != ESP_OK || e3 != ESP_OK || e4 != ESP_OK) {
        hw_note(it, "I2S 通道建立/使能失败");
        hw_end(it, HW_ST_FAIL);
    } else {
        /* 读 200ms 采样: 只要有数据流回来即证明 MCLK/BCLK/LRCK/DIN 通路在跑;
         * 全 0 也可能是"没焊麦克风", 只记录不判 */
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

    /* ② 时钟跑着的时候复读编解码器寄存器 */
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
