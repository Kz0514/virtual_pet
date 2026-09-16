/**
 * @file hw_devices.h
 * @brief C 段 六器件识别/通讯/配置 (raw i2c_master, 不依赖 app 驱动层)
 *        + F 段 音频通路 (I2S 建立 + 真 MCLK 下的编解码器回读)
 */
#ifndef HW_DEVICES_H
#define HW_DEVICES_H

#include <stdbool.h>

/* C 段: ES8311 / HDC1080 / OPT3001 / BQ27220 / MPU6500 / QMC6309(AUX) — 需总线已建 */
void hw_devices_run(void);
/* F 段: I2S 通道 + MCLK + 编解码器复读 (无声, 不出图案) — 需总线已建 */
void hw_audio_run(void);
/* D 段用: 当前 ES8311 关键寄存器是否仍与写入表一致 (给 hw_audio_run 复用) */
bool hw_devices_es_verify(const char *tag);

#endif /* HW_DEVICES_H */
