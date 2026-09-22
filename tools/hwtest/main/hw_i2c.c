/**
 * @file hw_i2c.c
 * @brief B 段 I2C 总线体检 (由 I2C 总线诊断工具移植而来)
 *
 * 判据来源 (别改着改着把结论丢了):
 *   [1][2] 空闲电平       → 外部 2.2K 上拉在不在 (SCL=GPIO0 是 strapping, 被钳就起不来)
 *   [3]    上升时间       → 与基准比对 (基准值见 HW_RISE_BASE_NS_*); 只靠内部 45K 会差一个数量级
 *   [4]    9 时钟+STOP    → 解卡能力 (被器件钳住的唯一软解)
 *   [5]    归因扫描       → 每笔后查电平, 指认"谁被访问后钳线"
 *   [13]   只读指纹       → 器件身份 (ES8311 无固定 ID, 用回读代替)
 *   [16]   间隔/节奏扫描  → 时间维度: 是否"打够密才咬"
 *   [17]   MCLK 开关      → ★决定性: 编解码器"有时钟才咬线" (半死 codec)
 *   [18]   双向拖动       → 焊桥/直流导通 vs 芯片数字行为
 *   [19]   MCLK 三态      → 楔子是否与时钟域有关
 *
 * ⚠️ 引脚归属陷阱: 位翻转段用 gpio_config(INPUT_OUTPUT_OD) 会摘掉外设输出路由,
 *    只有 i2c_new_master_bus 能恢复 → 见 hw_i2c.h 的调用顺序纪律。
 */
#include "hw_i2c.h"

#include <string.h>

#include "board.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_cpu.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hw_expect.h"
#include "hw_report.h"

static const char *TAG = "hwtest";

#define HW_SCL I2C_MASTER_SCL_IO
#define HW_SDA I2C_MASTER_SDA_IO
#define HW_SCAN_MAX 32
#define HW_PROBE_TIMEOUT_MS 10
#define HW_HAMMER_TIMEOUT_MS 50

/* 无内部上拉时应靠板载 2.2K 拉到高; 上升时间基准见下 */
static const uint32_t HW_RISE_BASE_NS_SCL = 383;
static const uint32_t HW_RISE_BASE_NS_SDA = 566;

static i2c_master_bus_handle_t s_bus = NULL;

/* 连打目标: 每地址读一个"读了没有副作用"的寄存器 (各自驱动里的读法) */
static const struct {
    uint8_t addr;
    uint8_t reg;
    uint8_t nbytes;
} HAMMER_TARGETS[] = {
    {0x18, 0x00, 2}, /* ES8311 */
    {0x40, 0xFF, 2}, /* HDC1080 设备 ID */
    {0x44, 0x7F, 2}, /* OPT3001 设备 ID */
    {0x55, 0x08, 2}, /* BQ27220 电压 */
    {0x68, 0x75, 1}, /* MPU6500 WHO_AM_I */
    {0x7C, 0x00, 2}, /* QMC6309 (AUX 旁路开的) */
};
#define HAMMER_TARGETS_N (sizeof(HAMMER_TARGETS) / sizeof(HAMMER_TARGETS[0]))

const char *hw_dev_name(uint8_t a)
{
    switch (a) {
    case 0x18: return "ES8311";
    case 0x7C: return "QMC6309";
    case 0x40: return "HDC1080";
    case 0x44: return "OPT3001";
    case 0x55: return "BQ27220";
    case 0x68: return "MPU6500";
    default:   return "?";
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * 时间基准 / 裸 GPIO (位翻转)
 * ══════════════════════════════════════════════════════════════════════ */
static uint32_t cycles_per_us(void)
{
    uint32_t t0 = esp_cpu_get_cycle_count();
    esp_rom_delay_us(1000);
    uint32_t c = esp_cpu_get_cycle_count() - t0;
    return c / 1000 ? c / 1000 : 1;
}

/* 开漏: 写 0 拉低, 写 1 释放 (靠上拉升回高) */
static void pin_od_init(gpio_num_t pin, bool internal_pullup)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = internal_pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(pin, 1);
}

/* 位翻转前: 夺走 pad (会摘掉外设输出路由)。
 * 注意 gpio_reset_pin 会连输入缓冲一起关掉 → gpio_get_level 恒 0, 之后所有
 * "线被按住"判断都是假的 (在正常板上原样复现过) — 所以只用开漏空闲态。 */
static void bb_arm(void)
{
    pin_od_init(HW_SCL, false);
    pin_od_init(HW_SDA, false);
}

/* 位翻转后把 pad 交还外设 (注意: 输出路由要 i2c_new_master_bus 才真正回来) */
static void peripheral_arm(void)
{
    pin_od_init(HW_SCL, false);
    pin_od_init(HW_SDA, false);
}

static int level_bits(void) { return (gpio_get_level(HW_SCL) << 1) | gpio_get_level(HW_SDA); }

bool hw_i2c_lines_free(void) { return level_bits() == 3; }

/* 释放后到"读为高"的周期数; UINT32_MAX = 拉不起来 */
static uint32_t rise_cycles(gpio_num_t pin, bool internal_pullup)
{
    pin_od_init(pin, internal_pullup);
    esp_rom_delay_us(200);
    uint32_t best = UINT32_MAX;
    for (int i = 0; i < 5; i++) {
        gpio_set_level(pin, 0);
        esp_rom_delay_us(20);
        uint32_t t0 = esp_cpu_get_cycle_count();
        gpio_set_level(pin, 1); /* 释放 */
        while (gpio_get_level(pin) == 0) {
            if (esp_cpu_get_cycle_count() - t0 > 2400000u) return UINT32_MAX; /* ≈10ms */
        }
        uint32_t c = esp_cpu_get_cycle_count() - t0;
        if (c < best) best = c;
    }
    return best;
}

static void bb_delay(void) { esp_rom_delay_us(4); } /* ≈125kHz */

static void bb_start(void)
{
    gpio_set_level(HW_SDA, 1); gpio_set_level(HW_SCL, 1); bb_delay();
    gpio_set_level(HW_SDA, 0); bb_delay();
    gpio_set_level(HW_SCL, 0); bb_delay();
}

static void bb_stop(void)
{
    gpio_set_level(HW_SDA, 0); bb_delay();
    gpio_set_level(HW_SCL, 1); bb_delay();
    gpio_set_level(HW_SDA, 1); bb_delay();
}

static bool bb_write_byte(uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(HW_SDA, (b >> i) & 1); bb_delay();
        gpio_set_level(HW_SCL, 1); bb_delay();
        gpio_set_level(HW_SCL, 0); bb_delay();
    }
    gpio_set_level(HW_SDA, 1); bb_delay(); /* 释放 SDA 读 ACK */
    gpio_set_level(HW_SCL, 1); bb_delay();
    bool ack = (gpio_get_level(HW_SDA) == 0);
    gpio_set_level(HW_SCL, 0); bb_delay();
    return ack;
}

/* 9 个 SCL 脉冲 + STOP, 标准总线解卡 (从机卡在半个字节里时唯一软解) */
static void bb_recover(void)
{
    gpio_set_level(HW_SDA, 1); bb_delay();
    for (int i = 0; i < 9; i++) {
        gpio_set_level(HW_SCL, 0); bb_delay();
        gpio_set_level(HW_SCL, 1); bb_delay();
    }
    bb_stop();
}

/* 只读一个寄存器 (位翻转): 写 addr|W, reg; 重复起始; addr|R, 读 n 字节 */
static bool bb_read_reg(uint8_t daddr, uint8_t reg, uint8_t *out, int nbytes)
{
    bb_start();
    if (!bb_write_byte((uint8_t)(daddr << 1)) || !bb_write_byte(reg)) { bb_stop(); return false; }
    bb_start(); /* 重复起始 */
    if (!bb_write_byte((uint8_t)((daddr << 1) | 1))) { bb_stop(); return false; }
    for (int k = 0; k < nbytes; k++) {
        uint8_t b = 0;
        for (int i = 7; i >= 0; i--) {
            gpio_set_level(HW_SCL, 1); bb_delay();
            b = (uint8_t)((b << 1) | (gpio_get_level(HW_SDA) ? 1 : 0));
            bb_delay();
            gpio_set_level(HW_SCL, 0); bb_delay();
        }
        out[k] = b;
        gpio_set_level(HW_SDA, (k == nbytes - 1) ? 1 : 0); /* 末字节 NACK */
        bb_delay();
        gpio_set_level(HW_SCL, 1); bb_delay();
        gpio_set_level(HW_SCL, 0); bb_delay();
    }
    gpio_set_level(HW_SDA, 1); bb_delay();
    bb_stop();
    return true;
}

/* 把 found[] 拼成 "0x18(ES8311) 0x40(HDC1080)" */
static void fmt_found(const uint8_t *found, int n, char *out, size_t out_len)
{
    if (n <= 0) { snprintf(out, out_len, "无"); return; }
    size_t used = 0;
    out[0] = '\0';
    for (int i = 0; i < n && used < out_len; i++) {
        int w = snprintf(out + used, out_len - used, "%s0x%02X(%s)", i ? " " : "", found[i],
                         hw_dev_name(found[i]));
        if (w < 0 || (size_t)w >= out_len - used) break;
        used += (size_t)w;
    }
}

/* 期望集合核对: 5 个直连器件必须全在; 0x7C(QMC6309, AUX 旁路开的) 允许出现。
 * 返回: 0 = 一致; 否则 *note 写原因 */
static int addrs_verify(const uint8_t *found, int n, char *note, size_t nl)
{
    bool want[HW_EXP_ADDRS_N] = {false};
    static const uint8_t want_addr[HW_EXP_ADDRS_N] = HW_EXP_ADDRS_INITIAL;
    int extra = 0;
    for (int i = 0; i < n; i++) {
        bool known = false;
        for (int k = 0; k < HW_EXP_ADDRS_N; k++) {
            if (found[i] == want_addr[k]) { want[k] = true; known = true; }
        }
        if (found[i] == HW_EXP_QMC_ADDR) known = true; /* 旁路开着时的地磁 */
        if (!known) extra++;
    }
    for (int k = 0; k < HW_EXP_ADDRS_N; k++) {
        if (!want[k]) {
            snprintf(note, nl, "缺 0x%02X(%s)", want_addr[k], hw_dev_name(want_addr[k]));
            return -1;
        }
    }
    if (extra) {
        /* 成片连续地址全"应答" = ACK 槽里 SDA 读成 0 —— 但 SDA 读 0 有两个来源:
         * 器件真钳住 还是 **根本没有上拉把线抬起来**(浮线读 0)。用 i2c.sdavolt 分辨。 */
        snprintf(note, nl, "多出 %d 个非预期应答地址 (成片连续 = ACK 槽读到低: 上拉缺失 或 被钳)",
                 extra);
        return -1;
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 * B1 空闲电平 + B2 上升时间/位翻转扫描
 * ══════════════════════════════════════════════════════════════════════ */
void hw_i2c_levels(void)
{
    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << HW_SCL) | (1ULL << HW_SDA),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in_cfg);
    esp_rom_delay_us(500);
    int scl_np = gpio_get_level(HW_SCL), sda_np = gpio_get_level(HW_SDA);

    in_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&in_cfg);
    esp_rom_delay_us(500);
    int scl_ip = gpio_get_level(HW_SCL), sda_ip = gpio_get_level(HW_SDA);

    hw_item_t *it = hw_begin("i2c.idle", "I2C 空闲电平");
    hw_set(it, "无上拉 SCL=%d SDA=%d | 内部上拉 SCL=%d SDA=%d", scl_np, sda_np, scl_ip, sda_ip);
    if (!scl_np && scl_ip) hw_note(it, "SCL 只靠内部 45K 才到高 → 板载 2.2K 可能缺失");
    if (!sda_np && sda_ip) hw_note(it, "SDA 只靠内部 45K 才到高 → 板载 2.2K 可能缺失");
    if (!scl_np && !scl_ip) hw_note(it, "SCL 恒低: 被器件钳住 或 上拉开路 (GPIO0 strapping!)");
    if (!sda_np && !sda_ip) hw_note(it, "SDA 恒低: 被器件钳住 或 上拉开路");
    hw_end(it, (scl_np && sda_np) ? HW_ST_PASS : HW_ST_FAIL);
}

void hw_i2c_bitbang(void)
{
    /* ── 上升时间 (无内部上拉, 只看外部 2.2K) ── */
    uint32_t cpu = cycles_per_us();
    uint32_t sc = rise_cycles(HW_SCL, false), sd = rise_cycles(HW_SDA, false);
    uint32_t sc_ns = (sc == UINT32_MAX) ? UINT32_MAX : sc * 1000 / cpu;
    uint32_t sd_ns = (sd == UINT32_MAX) ? UINT32_MAX : sd * 1000 / cpu;
    hw_item_t *it = hw_begin("i2c.rise", "I2C 上升时间");
    if (sc == UINT32_MAX || sd == UINT32_MAX) {
        /* 两边独立报: 一边正常一边死是重要信息 (缺上拉/被钳的是哪一根), 别打空串 */
        char s1[24], s2[24];
        if (sc == UINT32_MAX) snprintf(s1, sizeof(s1), "拉不起");
        else snprintf(s1, sizeof(s1), "%uns", (unsigned)sc_ns);
        if (sd == UINT32_MAX) snprintf(s2, sizeof(s2), "拉不起");
        else snprintf(s2, sizeof(s2), "%uns", (unsigned)sd_ns);
        hw_set(it, "SCL=%s SDA=%s", s1, s2);
        hw_note(it, "外部上拉开路 / 线被器件钳住 (用 i2c.sdavolt 分辨: 上拉缺失 vs 器件钳)");
        hw_end(it, HW_ST_FAIL);
    } else {
        hw_set(it, "SCL≈%uns SDA≈%uns (基准 %u/%u)", (unsigned)sc_ns, (unsigned)sd_ns,
               (unsigned)HW_RISE_BASE_NS_SCL, (unsigned)HW_RISE_BASE_NS_SDA);
        if (sc_ns > HW_EXP_RISE_MAX_NS || sd_ns > HW_EXP_RISE_MAX_NS) {
            hw_note(it, ">%uns: 上拉偏弱/容性负载异常", (unsigned)HW_EXP_RISE_MAX_NS);
            hw_end(it, HW_ST_FAIL);
        } else {
            hw_end(it, HW_ST_PASS);
        }
    }

    /* ── 位翻转扫描 @≈125kHz + 逐笔归因 (谁被访问后钳线) ── */
    bb_arm();
    int sc0 = gpio_get_level(HW_SCL), sd0 = gpio_get_level(HW_SDA);
    it = hw_begin("i2c.bbscan", "位翻转扫描 @125k");
    if (!sc0 || !sd0) {
        bb_recover();
        esp_rom_delay_us(200);
        hw_note(it, "扫描前已被钳线 (SCL=%d SDA=%d) → 9时钟后 SCL=%d SDA=%d", sc0, sd0,
                gpio_get_level(HW_SCL), gpio_get_level(HW_SDA));
        if (!hw_i2c_lines_free()) {
            hw_set(it, "起手即死");
            /* 没有上拉时 9 时钟也"解不开" (线靠什么回到高?) → 别一口咬定器件硬钳 */
            hw_note(it, "9 时钟也解不开 → 器件钳住 或 上拉缺失 (用 i2c.sdavolt 分辨)");
            hw_end(it, HW_ST_FAIL);
            peripheral_arm();
            return;
        }
    }

    uint8_t found[HW_SCAN_MAX];
    int n = 0;
    char stuck[96] = {0};
    size_t su = 0;
    for (uint16_t a = 0x08; a <= 0x7F; a++) {
        bb_start();
        bool ack = bb_write_byte((uint8_t)(a << 1));
        bb_stop();
        esp_rom_delay_us(20);
        if (ack && n < HW_SCAN_MAX) found[n++] = (uint8_t)a;
        if (!hw_i2c_lines_free() && su < sizeof(stuck) - 24) {
            su += (size_t)snprintf(stuck + su, sizeof(stuck) - su, "0x%02X ", (unsigned)a);
            bb_recover(); /* 解卡后继续, 否则后续全是假 ACK */
            esp_rom_delay_us(200);
        }
    }
    /* 顺手用位翻转真读一笔 (ACK ≠ 数据能读通): MPU WHO_AM_I */
    uint8_t who[1] = {0};
    bool who_ok = bb_read_reg(0x68, 0x75, who, 1);
    char list[160];
    fmt_found(found, n, list, sizeof(list));
    hw_set(it, "ACK=%d: %s", n, list); /* 一次成型: hw_set 是追加, 分两次会被截断 */
    hw_set(it, "| bb读 0x68/0x75=0x%02X", who_ok ? who[0] : 0);
    char why[64] = {0};
    if (stuck[0]) {
        hw_note(it, "访问后钳线: %s", stuck);
        hw_end(it, HW_ST_FAIL);
    } else if (addrs_verify(found, n, why, sizeof(why)) != 0) {
        hw_note(it, "%s", why);
        hw_end(it, HW_ST_FAIL);
    } else if (who_ok && who[0] != HW_EXP_MPU_WHO) {
        hw_note(it, "位翻转 WHO_AM_I=0x%02X ≠ 期望 0x%02X (外设路径为准, 见 dev.mpu)", who[0],
                HW_EXP_MPU_WHO);
        hw_end(it, HW_ST_WARN);
    } else {
        hw_end(it, HW_ST_PASS);
    }
    peripheral_arm();
}

/* ══════════════════════════════════════════════════════════════════════
 * 总线 (外设路径)
 * ══════════════════════════════════════════════════════════════════════ */
esp_err_t hw_i2c_bus_open(void)
{
    peripheral_arm();
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = HW_SDA,
        .scl_io_num = HW_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {.enable_internal_pullup = true},
    };
    return i2c_new_master_bus(&cfg, &s_bus);
}

void hw_i2c_bus_close(void)
{
    if (s_bus) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
}

i2c_master_bus_handle_t hw_i2c_bus(void) { return s_bus; }

/* 建临时设备 (用完即删, 不缓存句柄) */
static i2c_master_dev_handle_t dev_open(i2c_master_bus_handle_t bus, uint8_t addr, int khz)
{
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = (uint32_t)khz * 1000,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(bus, &dc, &dev) != ESP_OK) return NULL;
    return dev;
}

static int hw_scan(i2c_master_bus_handle_t bus, uint8_t *found)
{
    int n = 0;
    for (uint16_t a = 0x08; a <= 0x7F; a++) {
        /* 注意: i2c_master_probe() 内部固定 100kHz (与 scl_speed_hz 无关) */
        if (i2c_master_probe(bus, a, HW_PROBE_TIMEOUT_MS) == ESP_OK && n < HW_SCAN_MAX) {
            found[n++] = (uint8_t)a;
        }
    }
    return n;
}

void hw_i2c_bus_scan(void)
{
    hw_item_t *it = hw_begin("i2c.scan", "外设路径全地址扫描");
    if (!s_bus) {
        hw_note(it, "总线未建");
        hw_end(it, HW_ST_FAIL);
        return;
    }
    uint8_t found[HW_SCAN_MAX];
    int n = hw_scan(s_bus, found);
    char list[160];
    fmt_found(found, n, list, sizeof(list));
    hw_set(it, "ACK=%d: %s", n, list);
    char why[64] = {0};
    if (addrs_verify(found, n, why, sizeof(why)) != 0) {
        hw_note(it, "%s", why);
        hw_end(it, HW_ST_FAIL);
    } else if (!hw_i2c_lines_free()) {
        hw_note(it, "扫描后线被按住 SCL=%d SDA=%d", gpio_get_level(HW_SCL), gpio_get_level(HW_SDA));
        hw_end(it, HW_ST_FAIL);
    } else {
        hw_end(it, HW_ST_PASS);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * 连打/钳线归因
 * ══════════════════════════════════════════════════════════════════════ */
/* 静默 + 总线复位, 试把钳住的线放回来; 返回是否已放开 */
static bool recover_ladder(const char *who)
{
    if (hw_i2c_lines_free()) return true;
    /* ① 总线静默 — 这类半死芯片通常几十~几百 ms 后自己放开 */
    for (int i = 0; i < 40; i++) {
        vTaskDelay(pdMS_TO_TICKS(25));
        if (hw_i2c_lines_free()) {
            ESP_LOGW(TAG, "%s: 静默 %dms 后自己放开", who, (i + 1) * 25);
            return true;
        }
    }
    /* ② 外设总线复位 (9 时钟 + STOP) */
    esp_err_t e = s_bus ? i2c_master_bus_reset(s_bus) : ESP_ERR_INVALID_STATE;
    vTaskDelay(pdMS_TO_TICKS(20));
    bool ok = hw_i2c_lines_free();
    ESP_LOGW(TAG, "%s: 静默 1s 未放开 → i2c_master_bus_reset=%s → %s", who, esp_err_to_name(e),
             ok ? "放开" : "仍被按住");
    return ok;
}

/* 连打 n 笔读。返回: 0 = 全程未卡 / 1 = 卡线 (*at_iter = 第几笔) / 2 = 加设备失败
 * 错误码分开统计没意义 —— transmit_* 系列把 NACK 与超时都塌成 ESP_ERR_INVALID_STATE,
 * 所以"失败后线是否还被按着"才是判据 (见 i2c_diag.c:877-880 注释)。 */
static int hammer(i2c_master_bus_handle_t bus, uint8_t addr, uint8_t reg, int nbytes, int gap_us,
                  int rounds, int *ok, int *at_iter, const char *who)
{
    int okn = 0, at = 0;
    i2c_master_dev_handle_t dev = dev_open(bus, addr, 400);
    if (!dev) {
        ESP_LOGE(TAG, "%s: 加设备失败", who);
        *ok = 0;
        *at_iter = 0;
        return 2;
    }
    for (int i = 1; i <= rounds; i++) {
        uint8_t cmd = reg, buf[2] = {0, 0};
        if (i2c_master_transmit_receive(dev, &cmd, 1, buf, nbytes, HW_HAMMER_TIMEOUT_MS) == ESP_OK) {
            okn++;
        }
        if (!hw_i2c_lines_free()) { at = i; break; }
        if (gap_us > 0) esp_rom_delay_us((uint32_t)gap_us);
    }
    i2c_master_bus_rm_device(dev);
    *ok = okn;
    *at_iter = at;
    if (at) {
        ESP_LOGW(TAG, "%s: 第 %d 笔后钳线 (SCL=%d SDA=%d)", who, at, gpio_get_level(HW_SCL),
                 gpio_get_level(HW_SDA));
    }
    return at ? 1 : 0;
}

/* 每地址 400 笔 + 间隔扫描 + 0x7C↔0x55 交替 */
void hw_i2c_stress(void)
{
    if (!s_bus) {
        hw_bad("i2c.hammer", "满速连打压力", "总线未建");
        hw_bad("i2c.timing", "间隔/交替流量", "总线未建");
        return;
    }

    /* ── (a) 每地址 400 笔背靠背 ── */
    hw_item_t *it = hw_begin("i2c.hammer", "满速连打压力");
    char val[80] = {0};
    size_t vu = 0;
    bool any_wedged = false;
    int ok_total = 0; /* 全地址累计成功笔数: 0 = 线上根本没通讯 */
    for (size_t i = 0; i < HAMMER_TARGETS_N; i++) {
        int ok = 0, at = 0;
        char who[40];
        snprintf(who, sizeof(who), "0x%02X(%s)", HAMMER_TARGETS[i].addr,
                 hw_dev_name(HAMMER_TARGETS[i].addr));
        int r = hammer(s_bus, HAMMER_TARGETS[i].addr, HAMMER_TARGETS[i].reg,
                       HAMMER_TARGETS[i].nbytes, 0, HW_EXP_HAMMER_N, &ok, &at, who);
        int w = snprintf(val + vu, sizeof(val) - vu, "%s%02X:%d%s", vu ? " " : "",
                         HAMMER_TARGETS[i].addr, ok, (r == 1) ? "!" : (r == 2 ? "?" : ""));
        if (w > 0 && (size_t)w < sizeof(val) - vu) vu += (size_t)w;
        ok_total += ok;
        if (r == 2) {
            any_wedged = true;
            hw_note(it, "%s 加设备失败", who);
        } else if (r == 1) {
            any_wedged = true;
            hw_note(it, "%s 第 %d 笔后钳线", who, at);
            if (!recover_ladder(who)) break; /* 救不回来就别继续, 后面全是假的 */
        }
    }
    hw_set(it, "%s", val);
    /* ⚠️ "没卡线" ≠ "通过了": 线上 0 笔成功时本项什么都证明不了。
     * (实况: SDA 上拉缺失时全地址 0 笔, 旧版本却报 PASS。) */
    if (any_wedged) {
        hw_end(it, HW_ST_FAIL);
    } else if (ok_total == 0) {
        hw_note(it, "全地址 0/%d 笔成功 → 线上根本没有通讯, 别读成'压力通过' "
                    "(先看上拉/应答: i2c.sdavolt / dev.*)",
                (int)(HAMMER_TARGETS_N * HW_EXP_HAMMER_N));
        hw_end(it, HW_ST_FAIL);
    } else {
        hw_end(it, HW_ST_PASS);
    }

    /* ── (b) 间隔扫描 + 0x7C↔0x55 交替 ── */
    it = hw_begin("i2c.timing", "间隔/交替流量");
    if (ok_total == 0) {
        hw_note(it, "总线全程无应答 → 没有流量可测 (先解决上一项的失败)");
        hw_end(it, HW_ST_SKIP);
        return;
    }
    if (!hw_i2c_lines_free()) {
        hw_note(it, "起手线就被钳 → 跳过");
        hw_end(it, HW_ST_SKIP);
        return;
    }
    static const int gaps[] = {0, 200, 500, 1000, 2000};
    bool t_wedged = false;
    for (size_t i = 0; i < sizeof(gaps) / sizeof(gaps[0]) && !t_wedged; i++) {
        int ok = 0, at = 0;
        char who[40];
        snprintf(who, sizeof(who), "0x55 间隔%dus", gaps[i]);
        int r = hammer(s_bus, 0x55, 0x08, 2, gaps[i], 200, &ok, &at, who);
        if (r == 1) {
            t_wedged = true;
            hw_note(it, "0x55 间隔%dus 第 %d 笔后钳线", gaps[i], at);
            recover_ladder(who);
        } else if (r == 2) {
            t_wedged = true;
            hw_note(it, "0x55 加设备失败");
        }
    }
    if (!t_wedged) {
        /* 0x7C (旁路开的 QMC) 一笔 ↔ 0x55 一笔: 卡点落在谁身上就指认谁是触发者 */
        i2c_master_dev_handle_t d55 = dev_open(s_bus, 0x55, 400);
        i2c_master_dev_handle_t d0c = dev_open(s_bus, 0x7C, 400);
        int at = 0;
        uint8_t last = 0;
        if (d55 && d0c) {
            for (int i = 1; i <= HW_EXP_HAMMER_N; i++) {
                uint8_t cmd = 0x00, buf[2] = {0, 0};
                last = 0x7C;
                i2c_master_transmit_receive(d0c, &cmd, 1, buf, 2, HW_HAMMER_TIMEOUT_MS);
                if (!hw_i2c_lines_free()) { at = i; break; }
                cmd = 0x08;
                last = 0x55;
                i2c_master_transmit_receive(d55, &cmd, 1, buf, 2, HW_HAMMER_TIMEOUT_MS);
                if (!hw_i2c_lines_free()) { at = i; break; }
            }
        }
        if (d55) i2c_master_bus_rm_device(d55);
        if (d0c) i2c_master_bus_rm_device(d0c);
        if (at) {
            hw_note(it, "0x7C↔0x55 第 %d 对钳线, 那笔打的是 0x%02X", at, last);
            t_wedged = true;
            recover_ladder("0x7C↔0x55");
        }
    }
    hw_set(it, "间隔 0/200/500/1000/2000us ×200 笔 + 交替 %d 对", HW_EXP_HAMMER_N);
    hw_end(it, t_wedged ? HW_ST_FAIL : HW_ST_PASS);
}

/* ══════════════════════════════════════════════════════════════════════
 * 假 MCLK 开关 (LEDC 在 GPIO14 出 4.096MHz 方波)
 * 关键实验: 半死的 codec "MCLK 一跑 + 一笔总线活动"就钳 SDA,
 * 不跑时钟则只有最极端的 0x55 交替才咬得动。
 * ══════════════════════════════════════════════════════════════════════ */
esp_err_t hw_mclk_start(void)
{
    ledc_timer_config_t t = {
        .speed_mode = HW_LEDC_SPD,
        .duty_resolution = LEDC_TIMER_1_BIT,
        .timer_num = HW_MCLK_TIMER,
        .freq_hz = HW_MCLK_FAKE_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t e1 = ledc_timer_config(&t);
    ledc_channel_config_t c = {
        .gpio_num = AUDIO_MCLK_IO,
        .speed_mode = HW_LEDC_SPD,
        .channel = HW_MCLK_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = HW_MCLK_TIMER,
        .duty = 1,
        .hpoint = 0,
    };
    esp_err_t e2 = ledc_channel_config(&c);
    ledc_update_duty(HW_LEDC_SPD, HW_MCLK_CH);
    ESP_LOGI(TAG, "假 MCLK @GPIO%d: %u Hz, 定时器=%s 通道=%s", AUDIO_MCLK_IO,
             (unsigned)HW_MCLK_FAKE_FREQ_HZ, esp_err_to_name(e1), esp_err_to_name(e2));
    return (e1 == ESP_OK && e2 == ESP_OK) ? ESP_OK : ESP_FAIL;
}

void hw_mclk_stop(void)
{
    ledc_stop(HW_LEDC_SPD, HW_MCLK_CH, 0);
    gpio_reset_pin(AUDIO_MCLK_IO);
}

void hw_i2c_mclk_stress(void)
{
    hw_item_t *it = hw_begin("i2c.mclk", "MCLK 开关压力");
    if (!s_bus) {
        hw_note(it, "总线未建");
        hw_end(it, HW_ST_FAIL);
        return;
    }
    /* 起手先解一次 (上一段若留了钳位, 后面的判据全是假的) */
    if (!hw_i2c_lines_free() && !recover_ladder("MCLK 段起手")) {
        hw_note(it, "起手线被钳且解不开 → 判据无效");
        hw_end(it, HW_ST_FAIL);
        return;
    }

    if (hw_mclk_start() != ESP_OK) {
        hw_note(it, "LEDC 起不来");
        hw_end(it, HW_ST_FAIL);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    int ok = 0, at = 0;
    int r = hammer(s_bus, 0x18, 0x00, 2, 0, HW_EXP_HAMMER_N, &ok, &at, "0x18(ES8311)[有MCLK]");
    if (r == 1) {
        hw_note(it, "0x18 第 %d 笔后钳线 (无 MCLK 时同款连打不卡 → 半死 codec)", at);
        recover_ladder("0x18[有MCLK]");
    } else if (r == 2) {
        hw_note(it, "0x18 加设备失败");
    }

    /* MCLK 开着再全地址扫一遍: 坏件特征是"有时钟就不应答/钳线" */
    uint8_t found[HW_SCAN_MAX];
    int n = hw_scan(s_bus, found);
    char list[120];
    fmt_found(found, n, list, sizeof(list));
    hw_set(it, "有MCLK: 0x18 %d/%d 扫描 ACK=%d", ok, HW_EXP_HAMMER_N, n);
    if (n) hw_note(it, "%s", list);
    char why[64] = {0};
    int bad = addrs_verify(found, n, why, sizeof(why));
    if (bad) hw_note(it, "扫描: %s", why);
    if (!hw_i2c_lines_free()) hw_note(it, "线仍被按住");
    hw_mclk_stop();
    hw_end(it, (r != 0 || bad || !hw_i2c_lines_free()) ? HW_ST_FAIL : HW_ST_PASS);
}

/* ══════════════════════════════════════════════════════════════════════
 * B6 双向拖动 (纯电气): 焊桥 / ESD 击穿 vs 芯片数字行为
 * 桥接双向对称; "被时钟唤醒后自己驱动 SDA" 不会让 MCLK 脚跟着 SDA 变。
 * ⚠️ 会摘掉 pad 所有权 → 必须是最后一个碰 I2C 引脚的段。
 * ══════════════════════════════════════════════════════════════════════ */
static void mc_pin(gpio_mode_t mode, bool pullup)
{
    gpio_config_t c = {
        .pin_bit_mask = (1ULL << AUDIO_MCLK_IO),
        .mode = mode,
        .pull_up_en = pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&c);
}

void hw_i2c_bridge(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << HW_SCL) | (1ULL << HW_SDA),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    esp_rom_delay_us(500);

    hw_item_t *it = hw_begin("i2c.bridge", "MCLK↔SDA 双向拖动");
    /* 起手先解卡: 上一段的钳位会让下面全部无意义 (v2 就在这判错过) */
    bb_arm();
    bb_recover();
    gpio_config(&io);
    esp_rom_delay_us(500);
    int sda0 = gpio_get_level(HW_SDA);
    if (!sda0) {
        hw_set(it, "起手 SDA 被按住且 9 时钟解不开");
        hw_note(it, "判据无效 — 先解决钳线问题");
        mc_pin(GPIO_MODE_INPUT, false);
        bb_arm();
        peripheral_arm();
        hw_end(it, HW_ST_FAIL);
        return;
    }

    /* ① MCLK 高阻 + 弱上拉当哨兵 */
    mc_pin(GPIO_MODE_INPUT, true);
    esp_rom_delay_us(500);
    int mclk_sentinel = gpio_get_level(AUDIO_MCLK_IO);

    /* ② MCLK 输出低 → SDA 应被 2.2K 拉住; 被拖低就是有直流导通 */
    mc_pin(GPIO_MODE_OUTPUT, false);
    gpio_set_level(AUDIO_MCLK_IO, 0);
    esp_rom_delay_us(500);
    int sda_at_low = gpio_get_level(HW_SDA);

    /* ③ MCLK 输出高 → 纯电桥则 SDA 立刻回高 */
    gpio_set_level(AUDIO_MCLK_IO, 1);
    esp_rom_delay_us(500);
    int sda_at_high = gpio_get_level(HW_SDA);

    /* ④ 反向: MCLK 回高阻+弱上拉, 位翻转把 SDA 拉低, 看 MCLK 脚是否被拖低 */
    mc_pin(GPIO_MODE_INPUT, true);
    esp_rom_delay_us(200);
    bb_arm();
    gpio_set_level(HW_SDA, 0);
    esp_rom_delay_us(500);
    int mclk_at_low = gpio_get_level(AUDIO_MCLK_IO);

    /* 收尾: 还原成高阻无上拉 + pad 交还外设 */
    gpio_set_level(HW_SDA, 1);
    mc_pin(GPIO_MODE_INPUT, false);
    bb_arm();
    peripheral_arm();
    esp_rom_delay_us(500);

    hw_set(it, "MCLK哨兵=%d | ②MCLK低→SDA=%d | ③MCLK高→SDA=%d | ④SDA低→MCLK=%d", mclk_sentinel,
           sda_at_low, sda_at_high, mclk_at_low);
    bool conduct = !sda_at_low || !mclk_at_low;
    if (conduct) {
        hw_note(it, "检测到 %s 导通 → 焊桥/ESD 击穿 (MCLK 脚只挂 ES8311)",
                (!sda_at_low && !mclk_at_low) ? "双向" : (!sda_at_low ? "MCLK→SDA" : "SDA→MCLK"));
        hw_end(it, HW_ST_FAIL);
    } else if (!sda_at_high) {
        hw_note(it, "③ MCLK 拉高后 SDA 仍低 → 器件被时钟唤醒后抓住 SDA (9 时钟可解)");
        hw_end(it, HW_ST_WARN);
    } else {
        hw_end(it, HW_ST_PASS);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * B7 SDA 直流体检 (ADC 实测电压 → 等效电阻)
 *
 * 为什么单列一项: 前面所有项只能说出"SDA 坏了", 说不出**坏成什么样**。
 * SDA=GPIO1 正好落 ADC1_CH0 (S3 的 ADC 只覆盖 GPIO1..20), 于是能:
 *   ① 三种偏置下读线的真实电压 → 反推外部**上拉阻值**(2.2K 对不对, 不只是"在不在")
 *      和外部等效**下拉阻值**(器件漏电/半短路到底是几 K 还是几十 K)
 *   ② 推挽拉高/拉低 → 分辨"ESP32 自己的脚坏了"还是"外面有器件拖着"
 *      (脚坏: 拉不到低 / 外面没强下拉却拉不高; 器件拖: 拉低=0 但拉高被拽下来)
 *   ③ 空载 3 秒内多次采样 → 漂不漂 (热漂 = 半死的旁证)
 * ⚠️ 供电域异常 (供电脚虚焊 / 带病上电) 会造成 IO pad 永久损伤, 症状与
 *    "外面有器件钳线"几乎一样 → 本项的第 ② 组读数就是为分辨这两者设计的。
 * SCL=GPIO0 没有 ADC 功能 → 只能读逻辑电平做对照。
 * ⚠️ 纯引脚操作, 必须排在总线拆掉之后 (与位翻转/拖动段同级)。
 * ══════════════════════════════════════════════════════════════════════ */
static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_cali_handle_t s_cali = NULL;
static bool s_sda_suspect = false; /* B7 判出异常 → B8 才开量线窗口 */

bool hw_i2c_sda_suspect(void) { return s_sda_suspect; }

static int sda_mv(int n)
{
    int64_t sum = 0;
    int cnt = 0;
    for (int i = 0; i < n; i++) {
        int raw = 0, mv = 0;
        if (adc_oneshot_read(s_adc, ADC_CHANNEL_0, &raw) != ESP_OK) continue;
        if (s_cali) {
            if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) continue;
        } else {
            mv = raw * 3100 / 4095; /* 无校准曲线 → 线性估算 */
        }
        sum += mv;
        cnt++;
    }
    return cnt ? (int)(sum / cnt) : -1;
}

/* 配 SDA 脚 (输入/推挽输出, 内部上/下拉, 输出电平), 等线稳 */
static void sda_pad(gpio_mode_t mode, bool pu, bool pd, int level)
{
    gpio_config_t c = {
        .pin_bit_mask = (1ULL << HW_SDA),
        .mode = mode,
        .pull_up_en = pu ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = pd ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&c);
    if ((mode & GPIO_MODE_OUTPUT) != 0) gpio_set_level(HW_SDA, level);
    esp_rom_delay_us(3000);
}

/* 拿内部 45K 当已知负载反推外部等效电阻 (数量级; 内部阻值本身 30~80K) */
static float ext_k(int mv, bool internal_is_pullup)
{
    const float vdd = HW_EXP_VDD_MV;
    if (mv < 20 || mv > (int)vdd - 20) return -1.0f; /* 贴边 → 算不出 */
    float r = internal_is_pullup ? (float)mv / (vdd - (float)mv) : (vdd - (float)mv) / (float)mv;
    return HW_EXP_INT_PULL_K * r;
}

static void kfmt(char *out, size_t n, float k)
{
    if (k < 0) snprintf(out, n, "算不出");
    else if (k >= 1000.0f) snprintf(out, n, "%.1fM", k / 1000.0f);
    else snprintf(out, n, "%.1fK", k);
}

void hw_i2c_sda_probe(void)
{
    hw_item_t *it = hw_begin("i2c.sdavolt", "SDA 直流体检(ADC)");
    s_sda_suspect = true; /* 悲观起手: 只有最后判到 PASS 才清掉 */

    adc_oneshot_unit_init_cfg_t ucfg = {.unit_id = ADC_UNIT_1};
    if (adc_oneshot_new_unit(&ucfg, &s_adc) != ESP_OK) {
        hw_set(it, "ADC 单元初始化失败");
        hw_end(it, HW_ST_FAIL);
        return;
    }
    adc_oneshot_chan_cfg_t ccfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    adc_oneshot_config_channel(s_adc, ADC_CHANNEL_0, &ccfg);
    adc_cali_curve_fitting_config_t cal = {
        .unit_id = ADC_UNIT_1,
        .chan = ADC_CHANNEL_0,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    bool cali_ok = (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) == ESP_OK);
    if (!cali_ok) s_cali = NULL;

    /* 起手先放高阻 + 解一次卡 (前一段可能留了钳位) */
    sda_pad(GPIO_MODE_INPUT, false, false, 0);
    bb_arm();
    bb_recover();
    sda_pad(GPIO_MODE_INPUT, false, false, 0);

    int scl_ref = gpio_get_level(HW_SCL);

    int v_f = sda_mv(32);                     /* ① 空载 (无内部上拉/下拉) */
    int l_f = gpio_get_level(HW_SDA);
    sda_pad(GPIO_MODE_INPUT, true, false, 0); /* ② 内部上拉 45K */
    int v_pu = sda_mv(32);
    int l_pu = gpio_get_level(HW_SDA);
    sda_pad(GPIO_MODE_INPUT, false, true, 0); /* ③ 内部下拉 45K */
    int v_pd = sda_mv(32);
    int l_pd = gpio_get_level(HW_SDA);

    /* ③b 内部上拉+下拉**同时**开 → 自检: 没有外部件时应落在半轨 (≈VDD/2)。
     * 这条是给"探针读数可信吗"用的: 半轨 = 内部拉网络 + ADC 中点都正常, 于是
     * ③ 读到 0mV 只能解释成"线上真的没有上拉"。若线上有 2.2K, 此处会被抬到接近满轨。 */
    sda_pad(GPIO_MODE_INPUT, true, true, 0);
    int v_mid = sda_mv(32);

    gpio_set_drive_capability(HW_SDA, GPIO_DRIVE_CAP_3); /* 40mA */
    sda_pad(GPIO_MODE_INPUT_OUTPUT, false, false, 1);    /* ④ 推挽拉高 */
    int v_hi = sda_mv(32);
    sda_pad(GPIO_MODE_INPUT_OUTPUT, false, false, 0);    /* ⑤ 推挽拉低 */
    int v_lo = sda_mv(32);

    sda_pad(GPIO_MODE_INPUT, false, false, 0); /* ⑥ 空载漂移 */
    int dmin = 4000, dmax = -1;
    for (int i = 0; i < 6; i++) {
        int v = sda_mv(16);
        if (v >= 0) {
            if (v < dmin) dmin = v;
            if (v > dmax) dmax = v;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    sda_pad(GPIO_MODE_INPUT, false, false, 0); /* 收尾: 高阻 (后面各段管各自的脚) */
    adc_oneshot_del_unit(s_adc);
    s_adc = NULL;
    if (s_cali) {
        adc_cali_delete_scheme_curve_fitting(s_cali);
        s_cali = NULL;
    }

    float r_dn = ext_k(v_pu, true);  /* 内部上拉把线抬到 v_pu → 反推外部下拉 */
    float r_up = ext_k(v_pd, false); /* 内部下拉把线压到 v_pd → 反推外部上拉 */
    char s_dn[16], s_up[16];
    kfmt(s_dn, sizeof(s_dn), r_dn);
    kfmt(s_up, sizeof(s_up), r_up);

    hw_set(it, "mV: 空载%d 内上拉%d 内下拉%d 上下同开%d 推高%d 推低%d | 电平 %d%d%d | 算 上拉≈%s 下拉≈%s "
               "| 3s漂 %d~%d",
           v_f, v_pu, v_pd, v_mid, v_hi, v_lo, l_f, l_pu, l_pd, s_up, s_dn, dmin, dmax);
    hw_note(it, "SCL 对照电平=%d%s", scl_ref, cali_ok ? "" : " | 无校准曲线, 电压为线性估算");
    hw_note(it, "自检(上下同开)=%dmV ⇒ %s", v_mid,
            (v_mid >= HW_EXP_PAD_HI_MIN_MV)
                ? "外部上拉在 (2.2K 把线抬起来了)"
                : ((v_mid >= HW_EXP_INT_MID_MIN_MV)
                       ? "内部拉网络与 ADC 中点都正常, 线上无外部上拉"
                       : "偏低: 外部有下拉 或 引脚/ADC 可疑"));
    /* 这一条直接回答"是芯片内部坏了还是外面的事": 推挽两个方向都到位 = 脚没事 */
    bool pad_ok = (v_hi >= HW_EXP_PAD_HI_MIN_MV && v_lo <= HW_EXP_PAD_LO_MAX_MV);
    if (pad_ok) {
        hw_note(it, "推高/推低两侧都到位 (%dmV/%dmV) → ESP32 的 SDA 脚输出级正常, 故障在外面",
                v_hi, v_lo);
    }

    bool pad_lo_bad = (v_lo > HW_EXP_PAD_LO_MAX_MV);
    bool pad_hi_bad = (v_hi < HW_EXP_PAD_HI_MIN_MV);
    bool strong_dn = (r_dn > 0 && r_dn < HW_EXP_PULLDOWN_MIN_K);
    bool pu_missing = (v_pd < HW_EXP_SDA_FLOAT_MIN_MV);
    bool pu_weak = (r_up > 0 && r_up > HW_EXP_PULLUP_MAX_K);

    if (pad_lo_bad) {
        hw_note(it, "推挽也拉不到低 (%dmV) → ESP32 的 SDA 脚输出级坏", v_lo);
        hw_end(it, HW_ST_FAIL);
    } else if (pad_hi_bad && !strong_dn) {
        hw_note(it, "外面没有强下拉, 推挽却拉不高 (%dmV) → 怀疑 ESP32 的 SDA 脚高侧坏", v_hi);
        hw_end(it, HW_ST_FAIL);
    } else if (strong_dn) {
        hw_note(it, "SDA 被外部拖低 等效≈%s → 器件漏电/半短路 (推低实测 %dmV)", s_dn, v_lo);
        hw_end(it, HW_ST_FAIL);
    } else if (pu_missing) {
        /* 脚是好的 (上面那条) + 线上没有强下拉 + 内下拉/空载都读不到上拉 →
         * **芯片这一侧**看不到 2.2K。两种可能, 芯片读数**分不出来**, 得用表量板:
         *   ① 上拉电阻开路/未焊/走线断 (在网这一侧断)
         *   ② 芯片的 pad 与网络根本没连上 (模块 GPIO1 脚虚焊 / 走线断) —— 表在板上量
         *      电阻是 2.2K 好的时候, 就是这一种。B8 量线窗口就是为分辨它开的。 */
        hw_note(it, "外部上拉几乎不存在 (空载 %dmV, 内下拉下 %dmV, 上下同开 %dmV) → 芯片这一侧没有 "
                    "%.1fK; 要么那颗上拉开路, 要么芯片的脚与网络断开了 (量线窗口见 i2c.sdahold)",
                v_f, v_pd, v_mid, (double)HW_EXP_PULLUP_NOMINAL_K);
        hw_end(it, HW_ST_FAIL);
    } else if (pu_weak) {
        hw_note(it, "外部上拉偏弱 等效≈%s (期望 ≈%.1fK)", s_up, (double)HW_EXP_PULLUP_NOMINAL_K);
        hw_end(it, HW_ST_WARN);
    } else if (dmax - dmin > 200) {
        hw_note(it, "空载电压在漂 (%d~%dmV) → 有器件在缓慢变化 (热漂/半死)", dmin, dmax);
        hw_end(it, HW_ST_WARN);
    } else {
        s_sda_suspect = false;
        hw_end(it, HW_ST_PASS);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * B8 量线窗口: 把 SDA 钉在低 20 秒, 让人拿万用表在板上量"线跟不跟着芯片走"
 *
 * 为什么需要它: B7 只能证明"**芯片这一侧**没有 2.2K", 但"上拉电阻开路了"和
 * "芯片的脚与网络断开了 (模块脚虚焊/走线断)"在芯片读数上**一模一样** ——
 * 两种情况芯片都只看到"一个孤零零的 pad, 外面什么都没有"。
 * 把线钉低再量板, 立刻分开: 量到的点还是 3.3V ⇒ 该点与芯片之间断了;
 * 全 0V ⇒ 通路没问题 (回到查那颗上拉电阻)。
 * 只在 B7 判出异常时才开 (正常板不白等 20 秒); 本项无自动判据 → SKIP。
 * ══════════════════════════════════════════════════════════════════════ */
void hw_i2c_sda_hold(void)
{
    hw_item_t *it = hw_begin("i2c.sdahold", "SDA 拉低量线窗口");
    gpio_set_drive_capability(HW_SDA, GPIO_DRIVE_CAP_3);
    sda_pad(GPIO_MODE_INPUT_OUTPUT, false, false, 0); /* 钉低 */
    ESP_LOGW(TAG, "════ 量线窗口 %d 秒: SDA(GPIO%d) 正被芯片拉低 ════", HW_EXP_SDA_HOLD_S, HW_SDA);
    ESP_LOGW(TAG, "拿表量这三点, 都该是 0V: ①模块 GPIO%d 脚 ②上拉电阻的 SDA 端 ③任一器件 SDA 脚",
             HW_SDA);
    ESP_LOGW(TAG, "哪一点还是 3.3V ⇒ 断点就在它与芯片之间; 此刻 SCL(GPIO%d)=3.3V 可当表笔对照",
             HW_SCL);
    for (int i = 0; i < HW_EXP_SDA_HOLD_S * 10; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if ((i + 1) % 50 == 0) ESP_LOGW(TAG, "…… 还剩 %d 秒", HW_EXP_SDA_HOLD_S - (i + 1) / 10);
    }
    sda_pad(GPIO_MODE_INPUT, false, false, 0); /* 收手: 高阻 (后面各段管各自的脚) */
    ESP_LOGW(TAG, "════ 量线窗口结束: SDA 已放开 ════");
    hw_set(it, "已把 SDA 钉低 %d 秒后放开", HW_EXP_SDA_HOLD_S);
    hw_note(it, "人工量线项 (无自动判据): 若某点仍是 3.3V → 模块脚虚焊/走线断, 不是上拉电阻的事");
    hw_end(it, HW_ST_SKIP);
}
