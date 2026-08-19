/**
 * @file tm6604.c
 * @brief TM6604 线性马达 — GPIO38 EN + GPIO39 PWM (LEDC)
 */
#include "tm6604.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

static const char *TAG = "tm6604";

#define PIN_EN   38
#define PIN_PWM  39
#define LEDC_CH  LEDC_CHANNEL_1  /* ch0 used by backlight */
#define LEDC_SPD LEDC_LOW_SPEED_MODE
#define LEDC_TMR LEDC_TIMER_0
#define LEDC_FREQ 20000  /* 20 kHz, inaudible */

static TimerHandle_t s_stop_timer;
static volatile bool s_vibrating = false;

static void stop_cb(TimerHandle_t t) {
    ledc_set_duty(LEDC_SPD, LEDC_CH, 0);
    ledc_update_duty(LEDC_SPD, LEDC_CH);
    gpio_set_level(PIN_EN, 0);
    s_vibrating = false;
}

bool tm6604_is_vibrating(void) { return s_vibrating; }

esp_err_t tm6604_init(void) {
    gpio_config_t en_cfg = {
        .pin_bit_mask = BIT64(PIN_EN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&en_cfg);
    gpio_set_level(PIN_EN, 0);

    ledc_timer_config_t tmr = {
        .speed_mode = LEDC_SPD,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TMR,
        .freq_hz = LEDC_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&tmr);

    ledc_channel_config_t ch = {
        .gpio_num   = PIN_PWM,
        .speed_mode = LEDC_SPD,
        .channel    = LEDC_CH,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TMR,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&ch);

    s_stop_timer = xTimerCreate("vib_stop", pdMS_TO_TICKS(100), pdFALSE, NULL, stop_cb);

    ESP_LOGI(TAG, "TM6604 就绪 (EN=%d PWM=%d)", PIN_EN, PIN_PWM);
    return ESP_OK;
}

void tm6604_vibrate(uint8_t duty_pct, uint16_t duration_ms) {
    if (duty_pct > 100) duty_pct = 100;
    tm6604_vibrate_raw((uint16_t)((duty_pct * 1023) / 100), duration_ms);
}

void tm6604_vibrate_raw(uint16_t duty_raw, uint16_t duration_ms) {
    uint32_t duty = (duty_raw > 1023) ? 1023 : duty_raw;  /* 10-bit */

    ledc_set_duty(LEDC_SPD, LEDC_CH, duty);
    ledc_update_duty(LEDC_SPD, LEDC_CH);
    gpio_set_level(PIN_EN, 1);
    s_vibrating = true;

    xTimerChangePeriod(s_stop_timer, pdMS_TO_TICKS(duration_ms), 0);
    xTimerStart(s_stop_timer, 0);
}
