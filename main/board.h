/**
 * @file board.h
 * @brief Virtualpet 硬件引脚定义与板级常量
 *
 * SoC: ESP32-S3R8 | Flash: 32MB (W25Q256JVEIQ) | PSRAM: 8MB (片内)
 *
 * ⚠️ 关键硬件注意事项:
 *   GPIO0  — Strapping引脚(启动模式), I2C上拉2.2KΩ, 上电时必须为高电平
 *            如果某个I2C器件在上电时将SCL拉低, 设备将进入下载模式而无法启动
 *   GPIO46 — Strapping引脚(VDD_SPI电压选择), 板载10K下拉 → 上电VDD_SPI=3.3V
 *            上电完成后重新配置为PWM背光控制
 *   GPIO38 — 板载10K下拉, 马达在上电/复位期间保持禁用(安全设计)
 *   GPIO40 — 三合一中断线(MPU6500+OPT3001+BQ27220), 需开漏输出+外部上拉
 *            ISR中通过I2C依次查询各器件中断状态寄存器区分中断源
 */

#ifndef BOARD_H
#define BOARD_H

#include "hal/gpio_types.h"       /* GPIO_NUM_* 宏定义 */
#include "driver/i2c_master.h"    /* i2c_master_bus_handle_t 类型 */

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════════════════════════
 * I2C0 总线 (6个器件直连: ES8311, MPU6500, OPT3001, HDC1080, BQ27220)
 * QMC6309 不在此总线 — 它挂载在 MPU6500 的 AUX I2C 上, 通过 MPU6500 旁路访问
 * ════════════════════════════════════════════════════════════════════════ */
#define I2C_MASTER_SCL_IO           GPIO_NUM_0   /* ⚠️ Strapping引脚, 上电需高 */
#define I2C_MASTER_SDA_IO           GPIO_NUM_1
#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_FREQ_HZ          400000       /* 快速模式 400kHz (2.2K上拉, 短走线) */

/* I2C 器件地址 (7位地址) */
#define ES8311_I2C_ADDR             0x18         /* 音频编解码器 (直连I2C0) */
#define MPU6500_I2C_ADDR            0x68         /* 6轴加速度/陀螺仪 (直连I2C0, AD0=0) */
#define OPT3001_I2C_ADDR            0x44         /* 环境光传感器 (直连I2C0) */
#define HDC1080_I2C_ADDR            0x40         /* 温湿度传感器 (直连I2C0) */
#define QMC6309_I2C_ADDR            0x2C         /* 地磁传感器 (⚠️ 挂MPU6500 AUX总线, 非直连I2C0) */
#define BQ27220_I2C_ADDR            0x55         /* 电池电量计 (直连I2C0) */

/* ════════════════════════════════════════════════════════════════════════
 * 显示屏: ST7789 (SPI2, 4线制)
 * ════════════════════════════════════════════════════════════════════════ */
#define DISPLAY_SPI_HOST            SPI2_HOST
#define DISPLAY_MOSI_IO             GPIO_NUM_42   /* SPI 数据 (MOSI) */
#define DISPLAY_SCLK_IO             GPIO_NUM_43   /* SPI 时钟 */
#define DISPLAY_CS_IO               GPIO_NUM_45   /* SPI 片选 */
#define DISPLAY_DC_IO               GPIO_NUM_44   /* 数据/命令选择 */
#define DISPLAY_RST_IO              GPIO_NUM_41   /* 硬件复位 */
#define DISPLAY_BL_IO               GPIO_NUM_46   /* ⚠️ Strapping, 10K下拉, 上电后重配PWM */
#define DISPLAY_SPI_FREQ_HZ         (80 * 1000 * 1000)  /* SPI 80MHz */
#define DISPLAY_WIDTH               240
#define DISPLAY_HEIGHT              240

/* ════════════════════════════════════════════════════════════════════════
 * 触摸 FPC (12通道电容触摸, 使用ESP32-S3触摸外设)
 * ════════════════════════════════════════════════════════════════════════ */
/* --- 左侧: 1通道 功能键 --- */
#define TOUCH_CH_LEFT               GPIO_NUM_2

/* --- 顶部: 5通道 水平滑条 (左右选择/页面切换/摸头) --- */
#define TOUCH_CH_TOP_0              GPIO_NUM_3
#define TOUCH_CH_TOP_1              GPIO_NUM_4
#define TOUCH_CH_TOP_2              GPIO_NUM_5
#define TOUCH_CH_TOP_3              GPIO_NUM_6
#define TOUCH_CH_TOP_4              GPIO_NUM_7

/* --- 右侧: 6通道 垂直滑条 (音量/亮度/菜单上下选择) --- */
#define TOUCH_CH_RIGHT_0            GPIO_NUM_8
#define TOUCH_CH_RIGHT_1            GPIO_NUM_9
#define TOUCH_CH_RIGHT_2            GPIO_NUM_10
#define TOUCH_CH_RIGHT_3            GPIO_NUM_11
#define TOUCH_CH_RIGHT_4            GPIO_NUM_12
#define TOUCH_CH_RIGHT_5            GPIO_NUM_13

#define TOUCH_CH_COUNT              12      /* 总通道数 */
#define TOUCH_TOP_CH_COUNT          5       /* 顶部滑条通道数 */
#define TOUCH_RIGHT_CH_COUNT        6       /* 右侧滑条通道数 */
#define TOUCH_SAMPLE_PERIOD_MS      20      /* 采样周期 (50Hz) */

/* ════════════════════════════════════════════════════════════════════════
 * 音频: ES8311 编解码器 + TPA2011D1 功放
 * ════════════════════════════════════════════════════════════════════════ */
#define AUDIO_I2S_PORT              I2S_NUM_0
#define AUDIO_MCLK_IO               GPIO_NUM_14   /* 音频主时钟 */
#define AUDIO_DMIC_SCL_IO           GPIO_NUM_15   /* 数字麦克风时钟 */
#define AUDIO_ASDOUT_IO             GPIO_NUM_16   /* I2S DIN (来自麦克风) */
#define AUDIO_LRCK_IO               GPIO_NUM_17   /* I2S左右通道时钟 (WS) */
#define AUDIO_DSDIN_IO              GPIO_NUM_18   /* I2S DOUT (输出到编解码器) */
#define AUDIO_AMP_EN_IO             GPIO_NUM_21   /* TPA2011D1 功放使能 (高有效) */
#define AUDIO_SAMPLE_RATE           16000         /* 采样率 16kHz */
#define AUDIO_I2S_BITS_PER_SAMPLE   I2S_DATA_BIT_WIDTH_16BIT

/* ════════════════════════════════════════════════════════════════════════
 * 触觉: TM6604 X轴线性马达
 * ════════════════════════════════════════════════════════════════════════ */
#define HAPTIC_EN_IO                GPIO_NUM_38   /* 使能, 10K下拉(默认禁用) */
#define HAPTIC_PWM_IO               GPIO_NUM_39   /* PWM驱动信号 */
#define HAPTIC_PWM_FREQ_HZ          2000          /* PWM频率约2000Hz */

/* ════════════════════════════════════════════════════════════════════════
 * 传感器 (全部挂载I2C0)
 * ════════════════════════════════════════════════════════════════════════ */
#define SHARED_INT_IO               GPIO_NUM_40   /* MPU6500+OPT3001+BQ27220 共享中断 */
#define MPU6500_CS_IO               GPIO_NUM_NC   /* 使用I2C, 无需CS */

/* ════════════════════════════════════════════════════════════════════════
 * USB-C (原生USB OTG)
 * ════════════════════════════════════════════════════════════════════════ */
#define USB_DN_IO                   GPIO_NUM_19
#define USB_DP_IO                   GPIO_NUM_20

/* ════════════════════════════════════════════════════════════════════════
 * PSRAM 配置
 * ════════════════════════════════════════════════════════════════════════ */
#define PSRAM_SIZE_MB               8
#define PSRAM_ANIM_FRAME_POOL_KB    (3 * 1024)    /* 动画帧缓存池 3MB */

/* ════════════════════════════════════════════════════════════════════════
 * 电池
 * ════════════════════════════════════════════════════════════════════════ */
#define BATTERY_CAPACITY_MAH        800           /* 标称容量 */
#define BATTERY_LOW_THRESHOLD_PCT   10            /* 低电量阈值 10% */
#define BATTERY_CRITICAL_THRESHOLD_PCT 5          /* 极低电量阈值 5% */

/* ════════════════════════════════════════════════════════════════════════
 * 电源管理时序
 * ════════════════════════════════════════════════════════════════════════ */
#define IDLE_TIMEOUT_MS             30000              /* 30秒无操作 → 待机 */
#define SLEEP_TIMEOUT_MS            (5 * 60 * 1000)    /* 5分钟无操作 → 浅休眠 */
#define DEEP_SLEEP_TIMEOUT_MS       (30 * 60 * 1000)   /* 30分钟无操作 → 深度休眠 */

/* ── 函数声明 ── */

/**
 * @brief 获取I2C0总线句柄, 供各传感器驱动使用
 * @return I2C主总线句柄 (在 main.c 中初始化)
 */
i2c_master_bus_handle_t board_get_i2c_bus(void);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_H */
