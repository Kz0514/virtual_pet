/**
 * @file hw_expect.c
 * @brief hw_expect.h 里的表定义 (放 .c 免得每个 include 都生成一份 const 副本)
 */
#include "hw_expect.h"

/* 与 partitions.csv / 主工程同表 — 对不上就是烧错表/改错表, 直接 FAIL */
const hw_part_exp_t HW_EXP_PARTS[] = {
    {"nvs", "nvs", 0x9000, 0x6000},
    {"otadata", "ota", 0xf000, 0x2000},
    {"phy_init", "phy", 0x11000, 0x1000},
    {"ota_0", "ota_0", 0x20000, 0x200000},
    {"ota_1", "ota_1", 0x220000, 0x200000},
    {"nvskey", "nvs_keys", 0x420000, 0x4000},
    {"config", "littlefs", 0x424000, 0x80000},
    {"data", "fat", 0x4A4000, 0x100000},
    {"assets", "littlefs", 0x5A4000, 0x1A5C000},
};
const int HW_EXP_PARTS_N = sizeof(HW_EXP_PARTS) / sizeof(HW_EXP_PARTS[0]);

/* 主工程 mpu6500.c:87-104 的写入值 (顺序照 driver) */
const hw_reg_exp_t HW_EXP_MPU_REGS[] = {
    {0x6B, 0x01, "PWR_MGMT_1"},
    {0x19, 0x04, "SMPLRT_DIV"},
    {0x1A, 0x04, "CONFIG"},
    {0x1B, 0x00, "GYRO_CONFIG"},
    {0x1C, 0x08, "ACCEL_CONFIG"},
    {0x1D, 0x04, "ACCEL_CFG2"},
    {0x37, 0x02, "INT_PIN_CFG"},
};
const int HW_EXP_MPU_REGS_N = sizeof(HW_EXP_MPU_REGS) / sizeof(HW_EXP_MPU_REGS[0]);

/* ES8311 写入序列 (顺序照 codec 路径: es8311_open → config_sample → start) */
const hw_es_reg_exp_t HW_EXP_ES_SEQ[] = {
    {0x0D, 0xFA, "SYS_0D 上电缺省"},
    {0x44, 0x08, "GPIO_44 I2C抗扰"},
    {0x44, 0x08, "GPIO_44 二次写"},
    {0x01, 0x30, "CLK_01 初始"},
    {0x02, 0x00, "CLK_02"},
    {0x03, 0x10, "CLK_03 ADC_OSR"},
    {0x16, 0x24, "ADC_16 麦克增益"},
    {0x04, 0x10, "CLK_04 DAC_OSR"},
    {0x05, 0x00, "CLK_05 ADC/DAC分频"},
    {0x0B, 0x00, "SYS_0B"},
    {0x0C, 0x00, "SYS_0C"},
    {0x10, 0x1F, "SYS_10"},
    {0x11, 0x7F, "SYS_11"},
    {0x00, 0x80, "RESET_00 释放+从模式"},
    {0x01, 0x3F, "CLK_01 用外部MCLK"},
    {0x07, 0x00, "CLK_07 LRCK_H"},
    {0x08, 0xFF, "CLK_08 LRCK_L"},
    {0x06, 0x03, "CLK_06 BCLK分频(4-1)"},
    {0x17, 0xBF, "ADC_17 音量"},
    {0x0E, 0x02, "SYS_0E"},
    {0x12, 0x00, "SYS_12 开DAC"},
    {0x14, 0x1A, "SYS_14 模拟增益"},
    {0x0D, 0x01, "SYS_0D"},
    {0x15, 0x40, "ADC_15"},
    {0x37, 0x08, "DAC_37"},
    {0x45, 0x00, "GP_45"},
    /* ★ 主工程在 esp_codec_dev_open 之后手动覆盖的一条 (es8311_drv.c:205):
     * 库给 0x0D=0x01, 主工程改成 0x06 (VREF=1, VMID=normal)。
     * 这是 hwtest 与主工程 codec 终态**唯一**的已知差异 (12 个 DAC 侧寄存器实测比对过) */
    {0x0D, 0x06, "SYS_0D VREF/VMID (主工程覆盖)"},
};
const int HW_EXP_ES_SEQ_N = sizeof(HW_EXP_ES_SEQ) / sizeof(HW_EXP_ES_SEQ[0]);

/* 全部写完后的期望状态 (回读比对) */
const hw_es_reg_exp_t HW_EXP_ES_CMP[] = {
    {0x00, 0x80, "RESET_00"},
    {0x01, 0x3F, "CLK_01"},
    {0x02, 0x00, "CLK_02"},
    {0x03, 0x10, "CLK_03"},
    {0x04, 0x10, "CLK_04"},
    {0x05, 0x00, "CLK_05"},
    {0x06, 0x03, "CLK_06"},
    {0x07, 0x00, "CLK_07"},
    {0x08, 0xFF, "CLK_08"},
    {0x0B, 0x00, "SYS_0B"},
    {0x0C, 0x00, "SYS_0C"},
    {0x0D, 0x06, "SYS_0D"},
    {0x0E, 0x02, "SYS_0E"},
    {0x10, 0x1F, "SYS_10"},
    {0x11, 0x7F, "SYS_11"},
    {0x12, 0x00, "SYS_12"},
    {0x14, 0x1A, "SYS_14"},
    {0x15, 0x40, "ADC_15"},
    {0x16, 0x24, "ADC_16"},
    {0x17, 0xBF, "ADC_17"},
    {0x37, 0x08, "DAC_37"},
    {0x44, 0x08, "GPIO_44"},
    {0x45, 0x00, "GP_45"},
};
const int HW_EXP_ES_CMP_N = sizeof(HW_EXP_ES_CMP) / sizeof(HW_EXP_ES_CMP[0]);
