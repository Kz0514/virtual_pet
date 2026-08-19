/**
 * ES8311 板级驱动 — 基于 test_main/ 已验证代码
 *
 * 依赖: esp_codec_dev ^1.6.x
 * I2C 使用 board_get_i2c_bus() 共享总线 (外部2.2K上拉)
 */
#include "board.h"
#include "es8311_drv.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

static const char *TAG = "es8311";

/* PA (TPA2011D1) 开关 — 上电有爆音 (硬件遗留), 故开机开启后常开不再关闭 */

static struct {
    bool                   inited;
    es8311_drv_cfg_t       cfg;
    i2s_chan_handle_t      tx_chan;
    i2s_chan_handle_t      rx_chan;
    esp_codec_dev_handle_t adc_dev;
    esp_codec_dev_handle_t dac_dev;
    int                    frame_bytes;
} _es = {0};

/* ════════════════════════════════════════════════════════════════ */
esp_err_t es8311_drv_init(const es8311_drv_cfg_t *cfg)
{
    if (_es.inited) { ESP_LOGW(TAG, "Already init"); return ESP_OK; }

    _es.cfg = cfg ? *cfg : ES8311_DRV_DEFAULT_CFG();
    if (_es.cfg.sample_rate <= 0)     _es.cfg.sample_rate     = 16000;
    if (_es.cfg.bits_per_sample <= 0) _es.cfg.bits_per_sample = 16;
    if (_es.cfg.channels <= 0)        _es.cfg.channels        = 1;
    if (_es.cfg.frame_ms <= 0)        _es.cfg.frame_ms        = 20;

    _es.frame_bytes = _es.cfg.sample_rate * _es.cfg.channels
                    * (_es.cfg.bits_per_sample / 8) * _es.cfg.frame_ms / 1000;

    ESP_LOGI(TAG, "Init: %dHz %dbit %dch frame=%dms(%dB)",
             _es.cfg.sample_rate, _es.cfg.bits_per_sample, _es.cfg.channels,
             _es.cfg.frame_ms, _es.frame_bytes);

    /* ── PA EN ── */
    if (!_es.cfg.mic_only) {
        gpio_config_t pc = {.pin_bit_mask = BIT64(AUDIO_AMP_EN_IO), .mode = GPIO_MODE_OUTPUT,
                            .pull_down_en = GPIO_PULLDOWN_ENABLE};
        gpio_config(&pc);
        gpio_set_level(AUDIO_AMP_EN_IO, 0);
    }

    /* ── I2S 全双工 (大 DMA buffer 容忍 flash 总线阻塞) ── */
    i2s_chan_config_t ch = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    ch.dma_frame_num = 9600;  /* 200ms @48kHz, default ~2400 too small */
    esp_err_t ret = i2s_new_channel(&ch,
        _es.cfg.mic_only ? NULL : &_es.tx_chan, &_es.rx_chan);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "I2S chan fail: %d", ret); return ret; }

    i2s_std_config_t sc = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(_es.cfg.sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        _es.cfg.bits_per_sample,
                        _es.cfg.channels == 1 ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = AUDIO_MCLK_IO,   .bclk = AUDIO_DMIC_SCL_IO,
                      .ws   = AUDIO_LRCK_IO,    .dout = AUDIO_DSDIN_IO,
                      .din  = AUDIO_ASDOUT_IO },
    };

    if (!_es.cfg.mic_only) {
        ret = i2s_channel_init_std_mode(_es.tx_chan, &sc);
        if (ret != ESP_OK) { ESP_LOGE(TAG, "TX fail: %d", ret); return ret; }
        ret = i2s_channel_enable(_es.tx_chan);
        if (ret != ESP_OK) { ESP_LOGE(TAG, "TX en fail: %d", ret); return ret; }
    }
    ret = i2s_channel_init_std_mode(_es.rx_chan, &sc);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "RX fail: %d", ret); return ret; }
    ret = i2s_channel_enable(_es.rx_chan);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "RX en fail: %d", ret); return ret; }

    /* ── Codec 接口 ── */
    const audio_codec_data_if_t *rx_data, *tx_data = NULL;
    audio_codec_i2s_cfg_t rx_i2s = { .rx_handle = _es.rx_chan };
    rx_data = audio_codec_new_i2s_data(&rx_i2s);
    if (!_es.cfg.mic_only) {
        audio_codec_i2s_cfg_t tx_i2s = { .tx_handle = _es.tx_chan };
        tx_data = audio_codec_new_i2s_data(&tx_i2s);
    }

    audio_codec_i2c_cfg_t i2cc = {
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = board_get_i2c_bus(),
    };
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&i2cc);
    if (!ctrl) { ESP_LOGE(TAG, "ctrl_if fail"); return ESP_FAIL; }

    const audio_codec_gpio_if_t *gpio = audio_codec_new_gpio();
    if (!gpio) { ESP_LOGE(TAG, "gpio_if fail"); return ESP_FAIL; }

    /* ── ADC (录音) ── */
    es8311_codec_cfg_t adc_cfg = {
        .ctrl_if = ctrl, .gpio_if = gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC,
        .pa_pin = -1,  .use_mclk = true,
        .no_dac_ref = true,  /* ★ 断开内部 DAC→ADC 参考回路 */
    };
    const audio_codec_if_t *adc_codec = es8311_codec_new(&adc_cfg);
    if (!adc_codec) { ESP_LOGE(TAG, "ADC codec fail"); return ESP_FAIL; }

    _es.adc_dev = esp_codec_dev_new(&(esp_codec_dev_cfg_t){
        .codec_if = adc_codec, .data_if = rx_data, .dev_type = ESP_CODEC_DEV_TYPE_IN,
    });
    if (!_es.adc_dev) { ESP_LOGE(TAG, "ADC dev fail"); return ESP_FAIL; }

    /* ── DAC (播放) ── */
    if (!_es.cfg.mic_only) {
        /* pa_pin=-1: 库不接管 PA — es8311_pa_power 会在 open/enable 时把
         * PA 引脚重配为 GPIO 并打开 (初始化 POP 声的来源), 且使我们的
         * LEDC PWM 软上电彻底失效 (引脚被切回 GPIO, 占空比写不进去) */
        es8311_codec_cfg_t dac_cfg = {
            .ctrl_if = ctrl, .gpio_if = gpio,
            .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
            .pa_pin = -1, .use_mclk = true,
            .no_dac_ref = true,
        };
        const audio_codec_if_t *dac_codec = es8311_codec_new(&dac_cfg);
        if (!dac_codec) { ESP_LOGE(TAG, "DAC codec fail"); return ESP_FAIL; }

        _es.dac_dev = esp_codec_dev_new(&(esp_codec_dev_cfg_t){
            .codec_if = dac_codec, .data_if = tx_data, .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        });
        if (!_es.dac_dev) { ESP_LOGE(TAG, "DAC dev fail"); return ESP_FAIL; }
    }

    /* ── 打开设备 ── */
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = _es.cfg.sample_rate,
        .channel     = _es.cfg.channels,
        .bits_per_sample = _es.cfg.bits_per_sample,
    };

    esp_codec_dev_set_in_gain(_es.adc_dev, _es.cfg.mic_gain_db);

    ret = esp_codec_dev_open(_es.adc_dev, &fs);
    if (ret != ESP_CODEC_DEV_OK) { ESP_LOGE(TAG, "ADC open fail: %d", ret); return ESP_FAIL; }

    if (!_es.cfg.mic_only) {
        esp_codec_dev_set_out_vol(_es.dac_dev, 70);
        ret = esp_codec_dev_open(_es.dac_dev, &fs);
        if (ret != ESP_CODEC_DEV_OK) { ESP_LOGE(TAG, "DAC open fail: %d", ret); return ESP_FAIL; }
    }

    /* ★ 修正 esp_codec_dev 默认值: 0D=0x01→0x06 (VREF=1, VMID=normal) */
    esp_codec_dev_write_reg(_es.adc_dev, 0x0D, 0x06);

    /* 回读 I2S 格式寄存器 — 必须匹配 16-bit I2S */
    int r09, r0a;
    esp_codec_dev_read_reg(_es.adc_dev, 0x09, &r09);
    esp_codec_dev_read_reg(_es.adc_dev, 0x0A, &r0a);
    ESP_LOGI(TAG, "I2S fmt: SDP_IN=0x%02X SDP_OUT=0x%02X (期望 0x0C)", r09, r0a);

    /* 如果 SDP_OUT 不是 16-bit I2S (0x0C), 强制修正 */
    if ((r0a & 0x1F) != 0x0C) {
        esp_codec_dev_write_reg(_es.adc_dev, 0x0A, 0x0C);
        ESP_LOGW(TAG, "修正 SDP_OUT: 0x%02X → 0x0C (16-bit I2S)", r0a);
    }

    /* 回读更多关键寄存器 */
    int r01, r03, r14, r16, r18;
    esp_codec_dev_read_reg(_es.adc_dev, 0x01, &r01);
    esp_codec_dev_read_reg(_es.adc_dev, 0x03, &r03);
    esp_codec_dev_read_reg(_es.adc_dev, 0x14, &r14);
    esp_codec_dev_read_reg(_es.adc_dev, 0x16, &r16);
    esp_codec_dev_read_reg(_es.adc_dev, 0x18, &r18);
    ESP_LOGI(TAG, "Regs: 01=%02X 03=%02X 14=%02X 16=%02X 18=%02X (PGA/ALC)", r01, r03, r14, r16, r18);

    vTaskDelay(pdMS_TO_TICKS(200));

    _es.inited = true;
    ESP_LOGI(TAG, "Init OK");
    return ESP_OK;
}

/* ════════════════════════════════════════════════════════════════ */

int es8311_drv_read(int16_t *buf, int count) {
    if (!_es.inited || !_es.adc_dev) return -1;
    int bytes = count * sizeof(int16_t);
    int ret = esp_codec_dev_read(_es.adc_dev, (uint8_t *)buf, bytes);
    return (ret == ESP_CODEC_DEV_OK) ? bytes : ret;
}

int es8311_drv_write(const int16_t *buf, int count) {
    if (!_es.inited || !_es.dac_dev) return -1;
    int bytes = count * sizeof(int16_t);
    int ret = esp_codec_dev_write(_es.dac_dev, (uint8_t *)buf, bytes);
    return (ret == ESP_CODEC_DEV_OK) ? bytes : ret;
}

void es8311_drv_set_vol(int vol) {
    if (!_es.inited || !_es.dac_dev) return;
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    esp_codec_dev_set_out_vol(_es.dac_dev, vol);
}

void es8311_drv_set_mic_gain(float db) {
    if (!_es.inited || !_es.adc_dev) return;
    esp_codec_dev_set_in_gain(_es.adc_dev, db);
}

void es8311_drv_pa_set(bool on) {
    if (AUDIO_AMP_EN_IO < 0) return;
    gpio_set_direction(AUDIO_AMP_EN_IO, GPIO_MODE_OUTPUT);
    gpio_set_level(AUDIO_AMP_EN_IO, on ? 1 : 0);
    ESP_LOGI(TAG, "PA: %d", on ? 1 : 0);
}

void es8311_drv_dac_power(bool on) {
    if (!_es.inited || !_es.dac_dev) return;
    if (on) {
        esp_codec_dev_write_reg(_es.dac_dev, 0x12, 0x01);
    } else {
        esp_codec_dev_write_reg(_es.dac_dev, 0x12, 0x02);
    }
    ESP_LOGI(TAG, "DAC power: %d", on ? 1 : 0);
}

void es8311_drv_mute(bool mute) {
    if (!_es.inited || !_es.dac_dev) return;
    esp_codec_dev_set_out_mute(_es.dac_dev, mute);
    ESP_LOGI(TAG, "mute: %d", mute);
}

esp_codec_dev_handle_t es8311_get_dac_handle(void) { return _es.dac_dev; }

void es8311_drv_deinit(void) {
    if (!_es.inited) return;
    if (_es.dac_dev) { esp_codec_dev_close(_es.dac_dev); esp_codec_dev_delete(_es.dac_dev); }
    if (_es.adc_dev) { esp_codec_dev_close(_es.adc_dev); esp_codec_dev_delete(_es.adc_dev); }
    if (!_es.cfg.mic_only && _es.tx_chan) { i2s_channel_disable(_es.tx_chan); i2s_del_channel(_es.tx_chan); }
    if (_es.rx_chan) { i2s_channel_disable(_es.rx_chan); i2s_del_channel(_es.rx_chan); }
    memset(&_es, 0, sizeof(_es));
    ESP_LOGI(TAG, "Deinit done");
}
