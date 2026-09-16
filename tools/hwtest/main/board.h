/**
 * @file board.h
 * @brief hwtest 板级引脚表 — 抄自主工程 main/board.h (那边是唯一权威),
 *        硬件改版时两处都要改; 本文件不含任何 app 逻辑。
 *
 * SoC: ESP32-S3R8 | Flash: 32MB (W25Q256JVEIQ) | PSRAM: 8MB (片内)
 *
 * ⚠️ 关键硬件注意事项 (抄自主工程, 判读失败项时要看):
 *   GPIO0  — Strapping(启动模式), I2C 上拉 2.2K, 上电必须为高
 *            若某 I2C 器件上电时把 SCL 拉低 → 进下载模式起不来
 *   GPIO46 — Strapping(VDD_SPI 选择), 板载 10K 下拉 → 上电 VDD_SPI=3.3V
 *            上电完成后重配为 PWM 背光
 *   GPIO38 — 板载 10K 下拉, 马达上电/复位期间保持禁用 (安全设计)
 *   GPIO40 — 三合一中断线 (MPU6500+OPT3001+BQ27220), 开漏 + 外部上拉
 */
#ifndef HWTEST_BOARD_H
#define HWTEST_BOARD_H

#include "driver/gpio.h"
#include "driver/ledc.h"

/* ── I2C0 (5 器件直连; QMC6309 挂 MPU6500 的 AUX 上) ── */
#define I2C_MASTER_SCL_IO GPIO_NUM_0 /* ⚠️ Strapping, 上电需高 */
#define I2C_MASTER_SDA_IO GPIO_NUM_1
#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_MASTER_FREQ_HZ 400000 /* 快速模式 400kHz (2.2K 上拉, 短走线) */

#define ES8311_I2C_ADDR 0x18  /* 音频编解码器 */
#define MPU6500_I2C_ADDR 0x68 /* 6 轴 IMU (AD0=0) */
#define OPT3001_I2C_ADDR 0x44 /* 环境光 */
#define HDC1080_I2C_ADDR 0x40 /* 温湿度 */
#define BQ27220_I2C_ADDR 0x55 /* 电量计 (板上唯一 BGA) */
#define QMC6309_I2C_ADDR 0x0C /* 地磁 (AUX 旁路开时出现在 0x0C; 主工程 board.h 的 0x2C 是错的, 见 README) */

/* ── 显示屏 ST7789 (SPI2, 4 线制, 无 MISO → 只能验写通路) ── */
#define DISPLAY_SPI_HOST SPI2_HOST
#define DISPLAY_MOSI_IO GPIO_NUM_42
#define DISPLAY_SCLK_IO GPIO_NUM_43
#define DISPLAY_CS_IO GPIO_NUM_45
#define DISPLAY_DC_IO GPIO_NUM_44
#define DISPLAY_RST_IO GPIO_NUM_41
#define DISPLAY_BL_IO GPIO_NUM_46 /* ⚠️ Strapping, 10K 下拉, 上电后重配 PWM */
#define DISPLAY_SPI_FREQ_HZ (80 * 1000 * 1000)
#define DISPLAY_WIDTH 240
#define DISPLAY_HEIGHT 240
#define DISPLAY_MADCTL 0xA0 /* 270°CW, 供应商确认值 (st7789.c:110) */
#define DISPLAY_GAP_X 80    /* 控制器 320 列 → 活动区 240, 列偏移 80 */
#define DISPLAY_GAP_Y 0

/* ── 12 通道电容触摸 (ESP32-S3 触摸外设) ── */
#define TOUCH_CH_COUNT 12
#define TOUCH_TOP_CH_COUNT 5   /* GPIO3..7 */
#define TOUCH_RIGHT_CH_COUNT 6 /* GPIO8..13 */
#define TOUCH_LEFT_CH_COUNT 1  /* GPIO2 */

/* ── 音频: ES8311 + TPA2011D1 ── */
#define AUDIO_I2S_PORT I2S_NUM_0
#define AUDIO_MCLK_IO GPIO_NUM_14     /* 音频主时钟 (也是诊断用的"假 MCLK 开关"脚) */
#define AUDIO_DMIC_SCL_IO GPIO_NUM_15 /* BCLK */
#define AUDIO_ASDOUT_IO GPIO_NUM_16   /* I2S DIN (来自麦克风) */
#define AUDIO_LRCK_IO GPIO_NUM_17     /* WS */
#define AUDIO_DSDIN_IO GPIO_NUM_18    /* I2S DOUT (到编解码器) */
#define AUDIO_AMP_EN_IO GPIO_NUM_21   /* TPA2011D1 使能 (高有效) */
#define AUDIO_SAMPLE_RATE 48000       /* 与主工程 boot_init.c:234 一致 */
#define AUDIO_MCLK_HZ 12288000        /* 48000 × 256 (I2S_STD_CLK_DEFAULT_CONFIG) */

/* ── 触觉: TM6604 线性马达 ── */
#define HAPTIC_EN_IO GPIO_NUM_38  /* 使能, 板载 10K 下拉 (默认禁用) */
#define HAPTIC_PWM_IO GPIO_NUM_39 /* PWM 驱动 */
#define HAPTIC_PWM_FREQ_HZ 20000  /* ⚠️ 实跑 20kHz (tm6604.c:19); 主工程 board.h 注释写 2000Hz 是错的 */

/* ── 传感器共享中断 ── */
#define SHARED_INT_IO GPIO_NUM_40

/* ════════════════════════════════════════════════════════════════════════
 * hwtest 自己的 LEDC 分配 (主工程背光/马达共用 TIMER0; hwtest 分开, 互不干扰)
 * ════════════════════════════════════════════════════════════════════════ */
#define HW_LEDC_SPD LEDC_LOW_SPEED_MODE
#define HW_BL_TIMER LEDC_TIMER_0    /* 背光 — 同 st7789.c:31-52 */
#define HW_BL_CH LEDC_CHANNEL_0
#define HW_BL_FREQ_HZ 23814
#define HW_BL_RES LEDC_TIMER_10_BIT
#define HW_MCLK_TIMER LEDC_TIMER_1  /* 假 MCLK 开关 — 诊断手法, 见 hw_i2c.c */
#define HW_MCLK_CH LEDC_CHANNEL_1
#define HW_MCLK_FAKE_FREQ_HZ 4096000 /* 16kHz × 256, 只要"时钟在跑" */
#define HW_HAPTIC_TIMER LEDC_TIMER_2
#define HW_HAPTIC_CH LEDC_CHANNEL_2
#define HW_HAPTIC_RES LEDC_TIMER_10_BIT

/* ── 电池 ── */
#define BATTERY_CAPACITY_MAH 800

/* ── 屏幕看板配色 (RGB565) ── */
#define HW_C_BG 0x0000
#define HW_C_PASS 0x05E0
#define HW_C_FAIL 0xF800
#define HW_C_SKIP 0x7BEF
#define HW_C_WARN 0xFE60
#define HW_C_TEXT 0xFFFF
#define HW_C_DIM 0x8C71

#endif /* HWTEST_BOARD_H */
