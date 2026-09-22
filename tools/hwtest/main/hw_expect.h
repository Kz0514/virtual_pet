/**
 * @file hw_expect.h
 * @brief ★ 全部"配置真值"集中在此 —— 板子改版 / 期望值要改, 只动这个文件 + board.h。
 *
 * 判定语义 (见 hw_report.h):
 *   PASS = 实测与期望一致      FAIL = 不一致 / 器件不应答
 *   SKIP = 条件不满足(未烧 assets / 被开关关掉 / 通道没起来) —— 不算错
 *   WARN = 器件活着但数值不符预期(需人看一眼, 不判死)
 */
#ifndef HW_EXPECT_H
#define HW_EXPECT_H

#include <stdint.h>

#define HW_FW_VERSION "hwtest-1.2"

/* ═══ A. 系统 ═══ */
#define HW_EXP_FLASH_BYTES (32u * 1024 * 1024) /* W25Q256 */
#define HW_EXP_PSRAM_MIN_BYTES (7u * 1024 * 1024)
#define HW_EXP_PSRAM_MAX_BYTES (9u * 1024 * 1024)
#define HW_EXP_PSRAM_TEST_BYTES (1024u * 1024) /* 实际写-读校验 1MB */
#define HW_EXP_HEAP_FREE_MIN (64u * 1024)      /* 内部堆剩余下限 (测试时刻) */
#define HW_EXP_CHIP_CORES 2
#define HW_EXP_TEMP_MIN_C (-20)
#define HW_EXP_TEMP_MAX_C (90)

/* 分区表逐项核对 (与 partitions.csv / 主工程同表) */
typedef struct {
    const char *name;
    const char *subtype; /* 仅用于打印 */
    uint32_t offset;
    uint32_t size;
} hw_part_exp_t;

extern const hw_part_exp_t HW_EXP_PARTS[];
extern const int HW_EXP_PARTS_N;

/* ═══ B. I2C 总线 ═══ */
/* 上电空闲时两条线都必须是高 (无内部上拉, 靠板载 2.2K)。
 * 若某器件上电钳 SCL → GPIO0 strapping 也会被拉低 → 直接起不来, 所以这条最致命。 */
#define HW_EXP_ADDRS_INITIAL                                              \
    {                                                                     \
        0x18, 0x40, 0x44, 0x55, 0x68 /* QMC6309 挂 AUX, 单列一项 */      \
    }
#define HW_EXP_ADDRS_N 5
/* QMC6309: 挂 MPU6500 的 AUX, 靠 INT_PIN_CFG.BYPASS_EN 桥到主总线。
 * 手册 §5.4: 只有 1 个 I2C 地址, 默认 7CH (§8.2 时序图里 7 位地址 = 1111100)。
 * 主工程 board.h:41 的 0x2C 与曾见过应答的 0x0C 都不是它 → 只作对照探一下。 */
#define HW_EXP_QMC_ADDR 0x7C        /* 手册给的地址 */
#define HW_EXP_QMC_ADDR_ALT 0x0C    /* 曾见过应答的地址 */
#define HW_EXP_QMC_ADDR_LEGACY 0x2C /* 主工程里的常量 */
#define HW_EXP_RISE_MAX_NS 2000    /* 上升时间上限 (基准值见 hw_i2c.c) */
#define HW_EXP_HAMMER_N 400        /* 每地址背靠背连打笔数 */

/* B7 SDA 直流体检 (ADC 实测) 阈值 —— SDA=GPIO1 正好是 ADC1_CH0。
 * 板载上拉 2.2K; 内部上/下拉按 45K 名义值粗算 (实件散布 30~80K) → 阻值只报数量级。
 * 注意 ADC 12dB 衰减的满量程约 3100mV: 线接近 VDD 时会读成"饱和", 不是异常。 */
#define HW_EXP_VDD_MV 3300.0f       /* 分压粗算用的电源轨 (量出来才知道真值) */
#define HW_EXP_INT_PULL_K 45.0f     /* ESP32-S3 内部上/下拉名义阻值 */
#define HW_EXP_PULLUP_NOMINAL_K 2.2f
#define HW_EXP_PULLUP_MAX_K 3.2f    /* 等效上拉高于此 = 上拉偏弱/开路 */
#define HW_EXP_PULLDOWN_MIN_K 10.0f /* 等效下拉低于此 = 有器件在强拖低 (漏电/半短路) */
#define HW_EXP_PAD_HI_MIN_MV 2400   /* 推挽拉高低于此 = 脚高侧坏 或 外部强钳 */
#define HW_EXP_PAD_LO_MAX_MV 300    /* 推挽拉低高于此 = 脚低侧坏 */
#define HW_EXP_SDA_FLOAT_MIN_MV 150 /* "外部上拉几乎不存在"的判据 (内部下拉下测) */
#define HW_EXP_INT_MID_MIN_MV 1200  /* 内部上下拉"同开"的下限: 正常≈半轨; 更低 = 外部有下拉/引脚可疑 */
#define HW_EXP_SDA_HOLD_S 20        /* B7 判出异常时开的量线窗口秒数 (给人拿表量, 见 i2c.sdahold) */

/* ═══ C. 器件身份 / 配置 ═══ */
#define HW_EXP_HDC_ID 0x1050
#define HW_EXP_OPT_ID 0x3001
#define HW_EXP_OPT_MANUF 0x5449
/* OPT3001 配置: 主工程 opt3001.c:51 写入的字节 (连续测量 800ms + 自动量程)。
 * 注意 [15:12] RN 是自动量程的**只读回读**(随环境光变), 比对时必须掩掉 → 用 MASK。 */
#define HW_EXP_OPT_CFG_WRITE 0xCE10
#define HW_EXP_OPT_CFG_MASK 0x0FFF
/* BQ27220 DeviceType: TI 写法的 Control(0x0001) 在本硬件上三种写法都读回 0x0000
 * → 只记录不作判据, 身份改由"数值是否合理"判定 */
#define HW_EXP_BQ_DEVTYPE 0x0001
#define HW_EXP_MPU_WHO 0x70
#define HW_EXP_QMC_ID_REG 0x00 /* Chip ID, POR = 0x90 */
#define HW_EXP_QMC_ID 0x90
/* 0x09 只读状态: POR = 0x18 → D4 NVM_LOAD_DONE / D3 NVM_RDY 都是 1 (稳态)。
 * D2 ST_RDY(自检没跑=0) / D1 OVFL / D0 DRDY 随测量变 → 只判 D4/D3。 */
#define HW_EXP_QMC_ST_REG 0x09
#define HW_EXP_QMC_ST_MASK 0x18

/* ═══ C2. 加速度读数 (震动) ═══ */
/* 静止时 |a| 必须就是重力 1g —— 这条同时验了"传感器在产出物理上说得通的数据"。
 * 抖动只记录不判定: 手晃板子时它会抬起来, 静止时是本底噪声, 两者都正常。 */
#define HW_EXP_ACCEL_SAMPLES 50   /* 采样笔数 (间隔 10ms → 500ms 窗口) */
#define HW_EXP_ACCEL_MIN_G 0.70f  /* |a| 均值下/上限 */
#define HW_EXP_ACCEL_MAX_G 1.30f

/* BQ27220 合理性区间 (电压≈0 → FAIL: 电池是必需件) */
#define HW_EXP_BAT_VOLT_MIN_MV 3000
#define HW_EXP_BAT_VOLT_MAX_MV 4400
#define HW_EXP_BAT_NOPACK_MV 1000 /* 低于此值算"读不到" */
#define HW_EXP_BAT_TEMP_MIN_K 2400 /* 温度(0.1K) 合理区间: -33~87°C */
#define HW_EXP_BAT_TEMP_MAX_K 3600

/* MPU6500 配置期望: 主工程 mpu6500.c:85-104 的写入值, 回读必须一致 */
typedef struct {
    uint8_t reg;
    uint8_t val;
    const char *name;
} hw_reg_exp_t;

extern const hw_reg_exp_t HW_EXP_MPU_REGS[];
extern const int HW_EXP_MPU_REGS_N;

/* ES8311 配置期望 —— 由 hw_devices.c 按**主工程 codec 路径**顺序写入后回读。
 * 值来源: esp_codec_dev/device/es8311/es8311.c
 *   es8311_open() (:500-560) + es8311_config_sample() (:418-487) + es8311_start() (:270-330)
 * MCLK = 48000 × 256 = 12.288MHz (= I2S_STD_CLK_DEFAULT_CONFIG), 系数表行
 *   {12288000,48000, pre_div=1, mult=1, adc_div=1, dac_div=1, fs=0, lrck_h=0, lrck_l=0xff,
 *    bclk_div=4, adc_osr=0x10, dac_osr=0x10}
 * ES8311 没有固定 ID 寄存器 (0xFD/0xFE/0xFF 是 CHIP ID/VERSION, 无文档值),
 * 所以身份判据 = 寄存器写进去能读回来 + ID 三连读稳定非全 0/全 FF。
 * ★ 终点**不是**库的默认值: 主工程在 open 之后还手动覆盖了一条 0x0D=0x06 —
 *   main/drivers/audio/es8311_drv.c:205, 注释 "VREF=1, VMID=normal"。
 *   库给的是 0x01。复现主工程终态就必须照写这一条。 */
typedef struct {
    uint8_t reg;
    uint8_t val;
    const char *name;
} hw_es_reg_exp_t;

extern const hw_es_reg_exp_t HW_EXP_ES_SEQ[]; /* 按序写入 (照 codec 路径) */
extern const int HW_EXP_ES_SEQ_N;
extern const hw_es_reg_exp_t HW_EXP_ES_CMP[]; /* 写完全部后的期望状态, 逐条回读 */
extern const int HW_EXP_ES_CMP_N;
#define HW_EXP_ES_ID_REGS 0xFD /* CHIP ID1 / 0xFE ID2 / 0xFF VERSION 三连读 */

/* ═══ D. 面板 ═══ */
#define HW_EXP_BL_DUTY_PCT 50 /* 背光自检占空比 (10bit → 511), 之后关掉等看板 */

/* ═══ E. 触摸 ═══ */
/* 基线量程参考: 各通道基线的绝对高度天然散布很大 (几万量级都算正常), 所以
 * "高位"根本不是饱和: 真正的死通道是**恒 0**(断线)或打满 64k(短路), 别把中间值当阈值。 */
#define HW_EXP_TOUCH_MIN_RAW 2000  /* 低于此值 = 通道没读数 (断线/未焊接) */
#define HW_EXP_TOUCH_MAX_RAW 64000 /* 高于此值 = 打满/异常 */
#define HW_EXP_TOUCH_SAMPLES 8

/* ═══ F. 音频读数 ═══ */
/* 麦克风挂在编解码器 ADC 侧。判定不能用宽带 RMS —— 室温噪声/人声/空调都会抬高它,
 * 唯一确定的是"整窗一个样"(min==max) = ADC 没在采样。电平只报不判。 */
#define HW_EXP_MIC_MS 300 /* 采一窗的时长 */

/* 扬声器回采: 板子上喇叭和麦克风挨着 → "有没有出声"可以用声学回路自动判。
 * 判据取 **单频** 而不是宽带电平: 房间噪声/说话/空调都抬高宽带能量,
 * 只有这一个频点是**我们自己发出来的**。 */
#define HW_EXP_SPK_TONE_HZ 440.0f     /* 播放/回采的单音频率 */
#define HW_EXP_SPK_AMP 5000           /* 单音幅度 (LSB, 满 32767) ≈ -16dBFS */
#define HW_EXP_SPK_VOL_REG 0xCC       /* ES8311 0x32 = DAC 音量; app 播 TTS 时写的就是这个值 */
#define HW_EXP_SPK_LOOP_GAIN_MIN 4.0f /* 回采相对底噪的提升倍数下限 (先验值, 照结果回调没意义) */
#define HW_EXP_SPK_REPLAY_GAP_MS 1000 /* 播放与回放之间的静默: 让耳朵把两声分开 */

/* ═══ G. 马达 ═══ */
#define HW_EXP_HAPTIC_DUTY_PCT 50
/* 回环短震: 真通电震一下, 拿加速度计的峰抖动当"震没震"的判据。
 * 采样率要盖过线性马达的谐振频率 (通常 170~235Hz), 所以临时把 IMU 开到
 * 1kHz 输出 + 260Hz 带宽, 测完还原 —— 用默认的 21Hz 带宽会把谐振滤掉。 */
#define HW_EXP_BUZZ_DUTY_PCT 60
#define HW_EXP_BUZZ_SAMPLES 200  /* 每窗采样笔数 (1ms 一笔 → 约 200ms) */
#define HW_EXP_BUZZ_GAIN_MIN 3.0f /* 震中峰抖动 / 静止峰抖动 的下限 */
#define HW_EXP_BUZZ_MIN_G 0.03f   /* 峰抖动的绝对下限 (基线太平时的兜底) */
#define HW_EXP_BUZZ_BUSY_G 0.05f  /* 静止峰抖动高于此值 → 板子当时在被搬动 */

/* ═══ H. 存储 ═══ */
#define HW_EXP_CFG_SIZE 0x80000  /* 512KB */
#define HW_EXP_DATA_SIZE 0x100000 /* 1MB 分区 */
/* FAT 可见容量必然小于分区: wear_levelling 先吃掉一块 (1MB 分区上剩不到 256 扇区),
 * 再扣 FAT 表/根目录 → 可见容量通常落在 700KB 上下。
 * 所以只卡"明显缩水"的下限, 不跟分区大小比。 */
#define HW_EXP_DATA_MIN_BYTES (512u * 1024)
/* FAT12/16 的根目录是**固定表**: IDF 的 f_mkfs 传 n_root=0,
 * ff.c:5974 把它补成 512 项 → 根目录满时新建/建目录一律 FR_DENIED(=EACCES)。
 * 表里没有 0x00/0xE5 空位时, 新建文件/目录一律 FR_DENIED(=EACCES) —— 卷脏, 非硬件。
 * (卷几何别去读 ff.h 的 FATFS 字段: FF_FS_EXFAT 改字段宽度, csize 会读歪。) */
#define HW_EXP_FAT_ROOT_MAX 512
#define HW_EXP_ASSETS_SIZE 0x1A5C000 /* 26MB */
#define HW_EXP_NVS_FREE_MIN_PCT 15 /* NVS 空闲低于此比例 → WARN */

/* ═══ I. WiFi ═══ */
#define HW_EXP_WIFI_SCAN_TIMEOUT_MS 15000 /* 全信道扫描宽限 */
#define HW_EXP_WIFI_CONNECT_TIMEOUT_MS 25000 /* 连接 + DHCP 宽限 */

#endif /* HW_EXPECT_H */
