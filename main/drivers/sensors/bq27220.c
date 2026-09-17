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
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
 * SOC 失步兜底
 *
 * 芯片 SOC = RemainingCapacity/FullChargeCapacity, 两个量都靠库仑计数
 * 累积。TRM 明确本芯片**没有向上修正通路** — 基准一旦失步, 只能等
 * 「充电终止同步」(需电流跌到 Taper Current 以下, 本机运行中永不满足)
 * 或「重新初始化」(POR / BAT_INSERT / OCV_CMD) 才回得来。
 * 失步时读出的 SOC 与电压自相矛盾 (电压接近满格却报个位数百分比), 会静默
 * 关掉写盘闸。
 * 故此处用电压交叉校验, 矛盾时改由电压查表兜底。
 * ══════════════════════════════════════════════════════════════════════ */

/* 静置 OCV → SOC 粗对照 (锂电, 10% 一档)。仅失步兜底时使用。 */
static const struct { uint16_t mv; uint8_t pct; } k_ocv_tbl[] = {
    { 4200, 100 }, { 4100, 90 }, { 4000, 80 }, { 3930, 70 },
    { 3860,  60 }, { 3800, 50 }, { 3750, 40 }, { 3710, 30 },
    { 3670,  20 }, { 3600, 10 }, { 3450,  5 }, { 3300,  0 },
};
#define OCV_TBL_N ((int)(sizeof(k_ocv_tbl) / sizeof(k_ocv_tbl[0])))

uint16_t bq27220_soc_from_mv(uint16_t mv)
{
    if (mv >= k_ocv_tbl[0].mv) return 100;
    for (int i = 1; i < OCV_TBL_N; i++) {
        if (mv >= k_ocv_tbl[i].mv) {
            /* 落在 [i-1, i] 区间 → 线性插值 */
            uint16_t hi_mv = k_ocv_tbl[i - 1].mv, lo_mv = k_ocv_tbl[i].mv;
            int hi_p = k_ocv_tbl[i - 1].pct, lo_p = k_ocv_tbl[i].pct;
            return (uint16_t)(lo_p + (int)(mv - lo_mv) * (hi_p - lo_p) /
                                         (int)(hi_mv - lo_mv));
        }
    }
    return 0;
}

bool bq27220_soc_plausible(uint16_t soc_pct, uint16_t mv)
{
    if (mv < 3000) return true; /* 无电池/读不到 → 不参与判断, 交回芯片值 */
    if (mv >= 3900 && soc_pct <= 10) return false; /* 低向失步 */
    if (mv <= 3500 && soc_pct >= 90) return false; /* 高向失步 */
    return true;
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

/* ══════════════════════════════════════════════════════════════════════
 * 数据内存 (DM) 访问原语
 *
 * 活跃配置区 0x91B4–0x92D1; 容量字段 FCC=0x929D / DesignCap=0x929F (大端)。
 * 读: 设址 0x3E → 从 0x3E 读 n+2 字节, 前 2 字节是**地址回显**。
 * 写: 提交单位是整块 32 字节, 见 cfg_write_capacity 上方说明。
 * ══════════════════════════════════════════════════════════════════════ */

/* Control 子命令写: 一次 I2C 事务 [0x00][opcode LSB][opcode MSB]。
 * 子命令返回值在 MACData(0x40), 不在 Control(0x00) — TRM §2.2。 */
static esp_err_t ctrl_write(uint16_t op)
{
    uint8_t b[3] = { 0x00, (uint8_t)(op & 0xFF), (uint8_t)(op >> 8) };
    return i2c_master_transmit(s_dev, b, sizeof(b), 100);
}

/* 从 cmd 起连续读 n 字节 (块寄存器 0x40..0x5F 自动递增) */
static esp_err_t rd_bytes(uint8_t cmd, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &cmd, 1, buf, n, 200);
}

/* 写寄存器 cmd, 后跟 n 字节数据。
 * 缓冲区要容得下整块 BlockData 一次写 (0x40 后跟 32 字节 = 33), 故取 40。
 * 溢出守卫保留 — 宁可返回 INVALID_ARG 也不能越界。 */
static esp_err_t reg_write(uint8_t cmd, const uint8_t *d, size_t n)
{
    uint8_t b[40];
    if (n > sizeof(b) - 1) return ESP_ERR_INVALID_ARG;
    b[0] = cmd;
    memcpy(b + 1, d, n);
    return i2c_master_transmit(s_dev, b, n + 1, 100);
}

/* 安全等级: 3=Sealed 2=Unsealed 1=FullAccess (OperationStatus 0x3A/0x3B 的 SEC[1:0]) */
static int sec_level(void)
{
    uint16_t os = 0;
    if (reg_read(0x3A, &os) != ESP_OK) return -1;
    return (os >> 1) & 3;
}

/* 读数据内存: 选地址 → 从 0x3E 读 n+2 字节。
 * [0]=echo LSB, [1]=echo MSB 是地址回显 — 回显不等于请求值即寻址失败,
 * 这是唯一能区分"地址没选上"与"该地址本来就是 0"的手段。
 * 返回: 0=成功 1=I2C 层失败 2=回显不符 (写通路依赖同一寻址, 故分类统计)。*/
#define DM_OK     0
#define DM_I2C    1
#define DM_ECHO   2
static int dm_read_ex(uint16_t addr, uint8_t *out, size_t n, uint16_t *echo_out)
{
    uint8_t sel[2] = { (uint8_t)(addr & 0xFF), (uint8_t)(addr >> 8) };
    if (reg_write(0x3E, sel, sizeof(sel)) != ESP_OK) return DM_I2C;
    uint8_t buf[34];
    int last = DM_I2C;
    uint16_t echo = 0;
    for (int i = 0; i < 5; i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
        if (rd_bytes(0x3E, buf, n + 2) != ESP_OK) { last = DM_I2C; continue; }
        echo = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
        if (echo == addr) { memcpy(out, buf + 2, n); return DM_OK; }
        last = DM_ECHO;
    }
    if (echo_out) *echo_out = echo;
    return last;
}

static bool dm_read(uint16_t addr, uint8_t *out, size_t n)
{
    return dm_read_ex(addr, out, n, NULL) == DM_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 * 容量标尺改写 — 芯片 DM 出厂默认 DesignCapacity=3000 / FCC=2872 (ROM 默认),
 * 而本机电芯约 800mAh。SOC = RC/FCC 的**库仑计数**路径因此慢 3.6 倍
 * (OCV 重定基准路径是比值, 不受影响)。
 *
 * 覆盖相邻的 FCC(0x929D) 与 DesignCapacity(0x929F) 两个字段 (各 2 字节大端)。
 *
 * ⚠ DM 的提交单位是**整块 32 字节**, 不接受字段切片: 帧写出去 I2C 全 ACK,
 * 但块与 0x60/0x61 全无变化。芯片自己记的 MACDataLen(0x61) = 0x24 =
 * 2(地址) + 32(块数据) + 1(校验) + 1(长度), 与 TRM §6.1 示例的 Data_len 一致。
 * 故这里走"整块读 → 改两个字段 → 整块写回"。
 * 校验和 = 0xFF − (addrL + addrH + 全部 32 字节) — 地址两字节必须计入
 * (TRM §2.30; §6.1 示例写作"只算 BlockData", 是错的)。
 *
 * ⚠ DM 是 RAM 影子 (TRM §3.1) — 真 POR (拔电/换电池) 即回 ROM 出厂值。
 * 所以回读守卫不是优化而是**必需**: 每次冷启动都要重新落一遍标尺,
 * 否则 SOC 又按 3000mAh 的尺子算。
 * 副作用: 退出用 EXIT_CFG_UPDATE_REINIT 会让芯片按 OCV 重算一次 RC,
 * 所以写完 SOC 会跳一下, 属预期。
 *
 * ⚠ 失败只告警, 绝不 abort 启动 (bq27220_init 那层是 ESP_ERROR_CHECK)。
 * ══════════════════════════════════════════════════════════════════════ */
#define CFG_CAP_ADDR   0x929D
#define CFG_CAP_MAH    800
#define CFG_BLOCK_LEN  32      /* BlockData = 0x40..0x5F */

/* 数据内存块校验和: 0xFF − (地址两字节 + 全部数据字节) & 0xFF。
 * 地址字节必须计入 (TRM §2.30; §6.1 示例说"只算 BlockData"是错的)。 */
static uint8_t dm_cksum(uint16_t addr, const uint8_t *d, size_t n)
{
    uint32_t s = (uint32_t)(addr & 0xFF) + (uint32_t)(addr >> 8);
    for (size_t i = 0; i < n; i++) s += d[i];
    return (uint8_t)(0xFF - (s & 0xFF));
}

/* 进 FULL ACCESS — SLUUBD4A §6.1 说写数据内存要求 FULL ACCESS, 而本芯片上电
 * 是 UNSEAL 但未必是 FULL ACCESS。
 *
 * ⚠ 但**实测两组密钥都对 OpStatus 没有任何影响, 而写入照样成功** (见下方整块
 * 写路径), 说明本芯片不靠这一步。此函数保留为"万一"的兜底, 不是必需路径。
 *
 * 密钥对必须**无间断连发** (E2E: 中间被打断会让进级失败), 故两次 ctrl_write
 * 之间不插任何其它 I2C。
 * 两组密钥: Flipper 生产固件/§6.1 示例用 0x0414+0x3672; TRM §3.3 写 0x8000×2。
 * 密钥错芯片只是忽略, bq27 系无失败锁定。
 *
 * SEC 字段的字节序/位编码未确证, 故**两种位置的低两位都跟踪**: 只要任一处
 * 变化即判定密钥被接受。
 * ⚠ 绝不发 SEALED(0x0030) — 那才会把芯片锁死。 */
static bool cfg_try_full_access(uint8_t *sec_a, uint8_t *sec_b)
{
    static const uint16_t keys[2][2] = {
        { 0x0414, 0x3672 },
        { 0x8000, 0x8000 },
    };
    for (int k = 0; k < 2; k++) {
        ctrl_write(keys[k][0]);
        ctrl_write(keys[k][1]);       /* 紧接, 中间不留空隙 */
        vTaskDelay(pdMS_TO_TICKS(5));
        uint8_t os[2] = {0};
        if (rd_bytes(0x3A, os, sizeof(os)) != ESP_OK) continue;
        uint8_t a = os[0] & 3, b = os[1] & 3;
        ESP_LOGW(TAG, "容量配置: 密钥组%d (%04X/%04X) → OpStatus=%02X%02X SEC(低字节)=%u SEC(高字节)=%u",
                 k + 1, keys[k][0], keys[k][1], os[1], os[0], a, b);
        if (a != *sec_a || b != *sec_b) { *sec_a = a; *sec_b = b; return true; }
    }
    return false;
}

static esp_err_t cfg_write_capacity(void)
{
    uint8_t os[2] = {0};
    rd_bytes(0x3A, os, sizeof(os));
    uint8_t sec_a = os[0] & 3, sec_b = os[1] & 3;
    ESP_LOGI(TAG, "容量配置: 进配置态前 OpStatus=%02X%02X (SEC 低=%u 高=%u)",
             os[1], os[0], sec_a, sec_b);

    if (!cfg_try_full_access(&sec_a, &sec_b))
        ESP_LOGW(TAG, "容量配置: 两组密钥都没让 SEC 变化 — 或已是 FULL ACCESS, 或密钥不对");

    if (ctrl_write(0x0090) != ESP_OK) return ESP_FAIL;   /* ENTER_CFG_UPDATE */

    /* 轮询 CFGUPDATE 位, 不盲等 */
    bool entered = false;
    for (int i = 0; i < 30; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (rd_bytes(0x3A, os, sizeof(os)) == ESP_OK &&
            (((uint16_t)os[0] | ((uint16_t)os[1] << 8)) >> 10 & 1)) { entered = true; break; }
    }
    ESP_LOGI(TAG, "容量配置: 进配置态后 OpStatus=%02X%02X %s",
             os[1], os[0], entered ? "CFGUPDATE 已置起" : "超时");
    if (!entered) {
        ctrl_write(0x0092);   /* EXIT_CFG_UPDATE (不重初始化) */
        return ESP_FAIL;
    }

    /* 整块读 — 改块内字段必须先拿到其余 28 字节原值, 原样写回 */
    uint8_t blk[CFG_BLOCK_LEN];
    if (!dm_read(CFG_CAP_ADDR, blk, sizeof(blk))) {
        ESP_LOGW(TAG, "容量配置: 整块读失败, 放弃写入");
        ctrl_write(0x0092);
        return ESP_FAIL;
    }

    /* 写入门禁: 拿芯片自己记的 0x60 反验公式, 对不上一律不写 (防灌垃圾) */
    uint8_t c60 = 0, l61 = 0;
    rd_bytes(0x60, &c60, 1);
    rd_bytes(0x61, &l61, 1);
    uint8_t calc = dm_cksum(CFG_CAP_ADDR, blk, sizeof(blk));
    if (c60 != calc) {
        ESP_LOGW(TAG, "容量配置: 校验和公式不符 (芯片 %02X / 计算 %02X), 放弃写入", c60, calc);
        ctrl_write(0x0092);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "容量配置: 整块自验通过 — %uB 块, 芯片 60=%02X 61=%02X",
             (unsigned)sizeof(blk), c60, l61);

    blk[0] = (uint8_t)(CFG_CAP_MAH >> 8);  blk[1] = (uint8_t)(CFG_CAP_MAH & 0xFF);  /* FCC  */
    blk[2] = (uint8_t)(CFG_CAP_MAH >> 8);  blk[3] = (uint8_t)(CFG_CAP_MAH & 0xFF);  /* DCap */
    uint8_t cksum = dm_cksum(CFG_CAP_ADDR, blk, sizeof(blk));
    uint8_t flen  = (uint8_t)(2 + sizeof(blk) + 1 + 1);

    const uint8_t addr[2] = { (uint8_t)(CFG_CAP_ADDR & 0xFF), (uint8_t)(CFG_CAP_ADDR >> 8) };
    /* ⚠ 地址必须**一次事务**写 0x3E。
     * TI §6.1 示例把它画成 0x3E/0x3F 两个单字节写 —— **不能照做**: 那样芯片
     * 地址锁存会错乱, 随后钳住整条 I2C 总线 (本器件 I2C transaction timeout,
     * 同总线其它器件一起失联), 只能复位恢复。**0x3F 不要单独写**。 */
    esp_err_t e = reg_write(0x3E, addr, sizeof(addr));

    /* 诊断: 确认地址锁存与当前块缓冲 (读 0x3E 起 6 字节 = 回显2 + 数据4) */
    {
        uint8_t chk[6] = {0};
        if (rd_bytes(0x3E, chk, sizeof(chk)) == ESP_OK)
            ESP_LOGI(TAG, "容量配置: 设址后回显=%02X%02X 块首=%02X%02X%02X%02X",
                     chk[1], chk[0], chk[2], chk[3], chk[4], chk[5]);
    }

    /* ⚠ 块必须**一次事务**突发写 (0x40 后跟 32 字节)。拆成 32 次单字节写,
     * 芯片不认, 块缓冲一个字节都不变。读路径本来就是 34 字节连续事务, 长事务
     * 本器件扛得住。 */
    if (e == ESP_OK) e = reg_write(0x40, blk, sizeof(blk));
    if (e == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(1));                      /* 规范要求 ≥250µs */
        /* ⚠ 校验和与长度必须**在同一次 I2C 事务里**写入 (0x60 ← cksum, 0x61 ← len)。
         * 分两次写芯片不提交: 0x60 保持旧值、0x40 数据不变, 且全程无 I2C 错误 —
         * 是"看起来写成功了其实没有"的主要来源。 */
        const uint8_t tail[2] = { cksum, flen };
        e = reg_write(0x60, tail, sizeof(tail));
    }

    /* 写后立刻回读 — 字节落没落, 这一行直接判 */
    uint8_t b1[4] = {0}, c1 = 0, l1 = 0;
    rd_bytes(0x40, b1, sizeof(b1));
    rd_bytes(0x60, &c1, 1);
    rd_bytes(0x61, &l1, 1);
    ESP_LOGI(TAG, "容量配置: 写后 40块=%02X%02X%02X%02X 60=%02X 61=%02X (请求 60=%02X 61=%02X)",
             b1[0], b1[1], b1[2], b1[3], c1, l1, cksum, flen);

    vTaskDelay(pdMS_TO_TICKS(20));                         /* 规范要求 ≥10ms */
    ctrl_write(0x0091);                                    /* EXIT_CFG_UPDATE_REINIT */
    vTaskDelay(pdMS_TO_TICKS(50));

    if (e != ESP_OK) ESP_LOGW(TAG, "容量配置: 帧写入 I2C 失败: %s", esp_err_to_name(e));
    return e;
}

void bq27220_apply_capacity_cfg(void)
{
    if (!s_dev) return;

    /* 开机先读 DM — DM 是 RAM 影子, 真 POR 后会回到 ROM 出厂值, 所以这条
     * 日志正常就是"值不对 → 重写"。若某次冷启动它直接报目标值, 说明守卫
     * 提前生效过 (例如热复位: 电量计没断电, DM 还在)。 */
    uint8_t b[4];
    if (!dm_read(CFG_CAP_ADDR, b, sizeof(b))) {
        ESP_LOGW(TAG, "容量配置: 回读失败, 不动芯片");
        return;
    }
    uint16_t fcc = ((uint16_t)b[0] << 8) | b[1];
    uint16_t dcap = ((uint16_t)b[2] << 8) | b[3];
    ESP_LOGI(TAG, "容量配置: 开机读回 FCC=%umAh DesignCap=%umAh (目标 %u)",
             fcc, dcap, CFG_CAP_MAH);

    /* 芯片自报的关键量 (寄存器原值, 不经固件加工) — 标尺回退时这里的
     * FCC 会立刻显出来, 是判断"标尺到底生效没有"最快的一行。 */
    {
        uint16_t mv = 0, cur = 0, rc = 0, rfcc = 0, soc = 0;
        reg_read(0x08, &mv); reg_read(0x0C, &cur);
        reg_read(0x10, &rc); reg_read(0x12, &rfcc); reg_read(0x2C, &soc);
        ESP_LOGI(TAG, "容量配置: 芯片自报 %umV %dmA SOC=%u%% RC=%umAh FCC=%umAh (标尺=%u)",
                 mv, (int16_t)cur, soc, rc, rfcc, CFG_CAP_MAH);
    }

    if (fcc == CFG_CAP_MAH && dcap == CFG_CAP_MAH) {
        ESP_LOGI(TAG, "容量配置: 已是目标值, 不写");
        return;
    }
    if (cfg_write_capacity() != ESP_OK) return;

    if (dm_read(CFG_CAP_ADDR, b, sizeof(b)))
        ESP_LOGW(TAG, "容量配置: 写后回读 FCC=%umAh DesignCap=%umAh",
                 ((uint16_t)b[0] << 8) | b[1], ((uint16_t)b[2] << 8) | b[3]);
    else
        ESP_LOGW(TAG, "容量配置: 写后回读失败");
}

/* ══════════════════════════════════════════════════════════════════════
 * 观测探针 (TEMP, 全只读)
 *
 * /data/power_log.csv 只记 SOC 不记 RC, 于是两种病灶分不开:
 *   (a) RC 随电流正常累减, 只是被"满电同步"反复打回 FCC → 改 FCC/DCap 对症
 *   (b) RC 根本不随电流动 → 库仑计没在积分, 改标尺白搭, 得走重新初始化
 * 判据 = ΔRC 与 ∫I·dt 对不对得上。故把 RC/FCC/BatteryStatus 与自算的累计
 * 电荷一起打出来, 每 2s 一行。
 *
 * 但插着 USB 只能看到充电侧, RC 又恰好顶在 FCC 上 —— 观测不到"降"。
 * 故留一发 `OCV_CMD(0x000C)` 的挂点: 它强制按当前电压重算 DOD/RC, 与 POR
 * 同一条路径、**不写任何配置字节**, 用来把上面两种病灶分开
 * (默认关, 见 P1_OCV_AT)。
 *
 * 采样臂 = 独立任务 (不能在 boot_init 里阻塞: 要观测 20 分钟)。
 * BS 的位序按 bq27xxx 通用表 (bit0=DSG, bit3=FC), 存疑一律看原始 hex。
 * ══════════════════════════════════════════════════════════════════════ */
#define P1_SAMPLES 600          /* 600 × 2s = 20 分钟 */
/* OCV_CMD 那一发先关掉 — 它会重算 RC, 会盖掉容量写入本身的效果。
 * 想再发就把值调回 150 (第 150 点 = 开机后 300s)。 */
#define P1_OCV_AT  99999

static void p1_watch_task(void *arg)
{
    (void)arg;
    int64_t t0 = esp_timer_get_time(), tp = t0;
    double mah = 0.0;                    /* ∫I·dt, 充电为正 */
    uint16_t rc0 = 0, first = 1;

    for (int k = 0; k < P1_SAMPLES; k++) {
        if (k == P1_OCV_AT) {
            /* 强制按当前电压重算 DOD/RC — 与 POR 同一条路径, 不写任何配置。
             * 若 SOC 从 100% 掉到 ~82%(3.95V 在芯片自带的 DOD 表上的插值),
             * 即证 OCV/DOD 通路活着、100% 是被"满电同步"打上去的;
             * 若纹丝不动, 则是累加器本身不响应。 */
            uint16_t v0 = 0, s0 = 0, r0 = 0;
            reg_read(0x08, &v0); reg_read(0x2C, &s0); reg_read(0x10, &r0);
            esp_err_t e = ctrl_write(0x000C);   /* OCV_CMD */
            vTaskDelay(pdMS_TO_TICKS(50));
            uint16_t v1 = 0, s1 = 0, r1 = 0;
            reg_read(0x08, &v1); reg_read(0x2C, &s1); reg_read(0x10, &r1);
            ESP_LOGW(TAG, "★ 发出 OCV_CMD(0x000C) = %s", esp_err_to_name(e));
            ESP_LOGW(TAG, "★ 前: %umV SOC=%u%% RC=%umAh → 后: %umV SOC=%u%% RC=%umAh",
                     v0, s0, r0, v1, s1, r1);
        }

        uint16_t v = 0, i = 0, soc = 0, rc = 0, fcc = 0, bs = 0;
        reg_read(0x08, &v);  reg_read(0x0C, &i);   reg_read(0x2C, &soc);
        reg_read(0x10, &rc); reg_read(0x12, &fcc); reg_read(0x0A, &bs);

        int64_t tn = esp_timer_get_time();
        mah += (double)(int16_t)i * (tn - tp) / 1e6 / 3600.0;
        tp = tn;

        if (first) {
            first = 0; rc0 = rc; mah = 0.0;
            ESP_LOGI(TAG, "P1 起点: %umV %dmA SOC=%u%% RC=%umAh FCC=%umAh BS=0x%04X "
                          "(DSG=%u FC=%u)", v, (int)(int16_t)i, soc, rc, fcc, bs,
                     bs & 1, (bs >> 3) & 1);
        } else {
            ESP_LOGI(TAG, "P1 %4llds %umV %6dmA SOC=%3u%% RC=%umAh dRC=%+5d ∫I=%+6.1fmAh "
                          "FCC=%umAh BS=0x%04X",
                     (long long)((tn - t0) / 1000000), v, (int)(int16_t)i, soc, rc,
                     (int)rc - (int)rc0, mah, fcc, bs);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGW(TAG, "P1 观测结束 (%d 点) — 任务退出", P1_SAMPLES);
    vTaskDelete(NULL);
}

void bq27220_probe_watch(void)
{
    if (!s_dev) { ESP_LOGE(TAG, "未初始化, 跳过 P1 观测"); return; }

    ESP_LOGI(TAG, "══════ P1 观测轮 (全只读, 不写任何配置) ══════");

    /* 收尾核对 */
    uint16_t os = 0;
    reg_read(0x3A, &os);
    ESP_LOGI(TAG, "收尾: OpStatus=0x%04X SEC=%d CFGUPDATE=%d",
             os, sec_level(), (os >> 10) & 1);

    /* 目标帧回读 */
    {
        uint8_t b[4];
        if (dm_read(0x929D, b, sizeof(b)))
            ESP_LOGI(TAG, "目标帧 0x929D: %02X %02X %02X %02X "
                          "(FCC=%u, DesignCap=%u)", b[0], b[1], b[2], b[3],
                     (b[0] << 8) | b[1], (b[2] << 8) | b[3]);
        else
            ESP_LOGW(TAG, "目标帧 0x929D 读失败");
    }

    /* 整片 dump — 跨重启逐字节比对用, 后续写 FCC 的持久性核对要靠它 */
    ESP_LOGI(TAG, "dump 0x9180..0x92D1 (32B/行)");
    int rows = 0;
    for (uint16_t a = 0x9180; a <= 0x92D1; a += 0x20) {
        uint8_t b[32];
        memset(b, 0, sizeof(b));
        uint16_t ec = 0;
        int r = dm_read_ex(a, b, sizeof(b), &ec);
        if (r != DM_OK) {
            ESP_LOGW(TAG, "   %04X: 读失败 (%s, 回显 0x%04X)", a,
                     r == DM_I2C ? "I2C" : "回显不符", ec);
            continue;
        }
        rows++;
        char h[3 * 32 + 1];
        int p = 0;
        for (int i = 0; i < 32; i++)
            p += snprintf(h + p, sizeof(h) - p, "%02X ", b[i]);
        ESP_LOGI(TAG, "   %04X: %s", a, h);
    }
    ESP_LOGI(TAG, "dump %d 行 — 启动 P1 观测任务 (每 2s, 共 20 分钟)", rows);
    xTaskCreate(p1_watch_task, "bq_watch", 4096, NULL, 3, NULL);
}

