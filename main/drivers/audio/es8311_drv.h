/**
 * ES8311 板级驱动 — 基于 test_main 已验证的 esp_codec_dev 实现
 *
 * 默认配置: 16000Hz, 16bit, mono, 20ms帧
 *
 * 使用:
 *   es8311_drv_init(NULL);          // 默认配置
 *   es8311_drv_read(buf, 320);      // 读20ms录音数据
 *   es8311_drv_write(buf, 320);     // 播放
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sample_rate;        /* Hz, 默认 16000 */
    int bits_per_sample;    /* 16 或 32, 默认 16 */
    int channels;           /* 1=mono, 2=stereo, 默认 1 */
    int frame_ms;           /* 帧长 ms, 默认 20 */
    bool mic_only;          /* 仅录音, 默认 false */
    float mic_gain_db;      /* 初始麦克风增益 dB, 默认 30.0 */
} es8311_drv_cfg_t;

#define ES8311_DRV_DEFAULT_CFG()  \
    ((es8311_drv_cfg_t){           \
        .sample_rate     = 16000,  \
        .bits_per_sample = 16,     \
        .channels        = 1,      \
        .frame_ms        = 20,     \
        .mic_only        = false,  \
        .mic_gain_db     = 30.0f,  \
    })

/** 初始化 (cfg=NULL 使用默认值). 共享I2C总线须已在 board 层初始化. */
esp_err_t es8311_drv_init(const es8311_drv_cfg_t *cfg);

/** 读取录音数据, 返回字节数 (<0=错误). count = sr*ch*frame_ms/1000 */
int  es8311_drv_read(int16_t *buf, int count);

/** 写入播放数据, 返回字节数 */
int  es8311_drv_write(const int16_t *buf, int count);

/** 播放音量 0~100 */
void es8311_drv_set_vol(int vol);

/** 麦克风增益 0~42 dB (step 6dB: 0,6,12,18,24,30,36,42) */
void es8311_drv_set_mic_gain(float db);

/** 功放开关 */
void es8311_drv_pa_set(bool on);

/** 硬件静音 (寄存器0x31 mute bit) — 播放结束调用消除残余噪音 */
void es8311_drv_mute(bool mute);

#include "esp_codec_dev.h"
/** 获取 DAC 句柄 (用于直接写寄存器) */
esp_codec_dev_handle_t es8311_get_dac_handle(void);

/** 释放资源 */
void es8311_drv_deinit(void);

#ifdef __cplusplus
}
#endif
