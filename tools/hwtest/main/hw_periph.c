/**
 * @file hw_periph.c
 * @brief D/E/G 段 —— 面板写通路, 触摸 12 通道, 马达脚
 *
 * 抄录来源 (改硬件时对账这两边):
 *   D: main/drivers/display/st7789.c:29-127 (LEDC 背光 23814Hz/10bit, MADCTL 0xA0,
 *      gap(80,0), FRCTRL2 0xC6=0x00, invert)
 *   E: main/drivers/touch/touch_fpc.c:30-43 (通道表), 564-581 (init), 145-200 (校准)
 *   G: main/drivers/haptic/tm6604.c:19 (20kHz), EN=GPIO38 板载 10K 下拉
 */
#include "hw_periph.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/touch_sensor.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hw_expect.h"
#include "hw_report.h"

static const char *TAG = "hwtest";

static esp_lcd_panel_io_handle_t s_io = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static SemaphoreHandle_t s_done = NULL;

/* ── 脚自检: 驱动 1 读到 1 / 驱动 0 读到 0 ──
 * 只能抓"短路到地/到 VCC"; 脚开路时读回值仍跟随驱动 (纯 pad 无负载) → 抓不到开路。 */
static bool pad_follows(gpio_num_t io, char *out, size_t out_len)
{
    gpio_config_t c = {
        .pin_bit_mask = 1ULL << (int)io,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&c) != ESP_OK) {
        if (out) snprintf(out, out_len, "GPIO%d 配置失败", (int)io);
        return false;
    }
    gpio_set_level(io, 1);
    esp_rom_delay_us(300);
    int hi = gpio_get_level(io);
    gpio_set_level(io, 0);
    esp_rom_delay_us(300);
    int lo = gpio_get_level(io);
    if (out) snprintf(out, out_len, "高=%d 低=%d", hi, lo);
    return hi == 1 && lo == 0;
}

/* ══════════════════════════════════════════════════════════════════════
 * D 段: 面板
 * ══════════════════════════════════════════════════════════════════════ */
static bool IRAM_ATTR lcd_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e,
                                     void *ctx)
{
    (void)io;
    (void)e;
    (void)ctx;
    BaseType_t hp = pdFALSE;
    if (s_done) xSemaphoreGiveFromISR(s_done, &hp);
    return hp == pdTRUE;
}

esp_err_t hw_lcd_get(esp_lcd_panel_handle_t *panel, esp_lcd_panel_io_handle_t *io)
{
    if (!s_panel) return ESP_ERR_INVALID_STATE;
    if (panel) *panel = s_panel;
    if (io) *io = s_io;
    return ESP_OK;
}

esp_err_t hw_lcd_blit(const void *fb)
{
    if (!s_panel || !s_done) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_done, 0); /* 清掉上一帧的残留 give */
    esp_err_t e = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, fb);
    if (e != ESP_OK) return e;
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(2000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    return ESP_OK;
}

void hw_periph_lcd(void)
{
    ESP_LOGI(TAG, "──── D 段: 面板 SPI 写通路 + 背光 ────");

    /* ① RST / DC 脚自检 (趁面板驱动还没接管它们) */
    hw_item_t *it = hw_begin("lcd.pins", "面板 RST/DC 脚");
    char a[40], b[40];
    bool ok1 = pad_follows(DISPLAY_RST_IO, a, sizeof(a));
    bool ok2 = pad_follows(DISPLAY_DC_IO, b, sizeof(b));
    hw_set(it, "RST %s / DC %s", a, b);
    if (!ok1 || !ok2)
        hw_note(it, "脚不跟随驱动 = 短路到地/VCC 或 pad 损坏");
    hw_end(it, (ok1 && ok2) ? HW_ST_PASS : HW_ST_FAIL);

    /* ② 背光 LEDC (先于屏幕点亮; duty 回读 + 脚自检)
     * ⚠️ 脚自检必须在 ledc_channel_config 之前: gpio_config() 会把外设输出路由
     * 从 pad 上摘掉 (见 hw_i2c.h 顺序纪律), 之后 LEDC 未必把背光脚接回去。 */
    it = hw_begin("lcd.bl", "背光 PWM");
    char c[40];
    bool pad_ok = pad_follows(DISPLAY_BL_IO, c, sizeof(c));
    ledc_timer_config_t t = {
        .speed_mode = HW_LEDC_SPD,
        .duty_resolution = HW_BL_RES,
        .timer_num = HW_BL_TIMER,
        .freq_hz = HW_BL_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t e1 = ledc_timer_config(&t);
    ledc_channel_config_t ch = {
        .gpio_num = DISPLAY_BL_IO,
        .speed_mode = HW_LEDC_SPD,
        .channel = HW_BL_CH,
        .timer_sel = HW_BL_TIMER,
        .duty = 0,
        .hpoint = 0,
        .intr_type = LEDC_INTR_DISABLE,
    };
    esp_err_t e2 = ledc_channel_config(&ch);
    /* 50% → 回读必须是 511 (10bit) */
    uint32_t want = (uint32_t)HW_EXP_BL_DUTY_PCT * 1023 / 100;
    esp_err_t e3 = ledc_set_duty(HW_LEDC_SPD, HW_BL_CH, want);
    e3 |= ledc_update_duty(HW_LEDC_SPD, HW_BL_CH);
    /* ledc_get_duty 读的是硬件 duty_rd 回读寄存器 (ledc_ll_get_duty: duty_read>>4),
     * 它由 PWM 在**下一个周期**锁存 → 立刻读只能拿到上一拍的旧值, 必须等一下 */
    vTaskDelay(pdMS_TO_TICKS(20));
    uint32_t got = ledc_get_duty(HW_LEDC_SPD, HW_BL_CH);
    uint32_t freq = ledc_get_freq(HW_LEDC_SPD, HW_BL_TIMER);
    ledc_set_duty(HW_LEDC_SPD, HW_BL_CH, 0); /* 看板画完再由 main 打开 */
    ledc_update_duty(HW_LEDC_SPD, HW_BL_CH);
    hw_set(it, "%uHz duty 回读 %u/%u, 脚 %s", (unsigned)freq, (unsigned)got, (unsigned)want, c);
    if (e1 != ESP_OK || e2 != ESP_OK || e3 != ESP_OK) {
        hw_note(it, "LEDC 配置失败");
        hw_end(it, HW_ST_FAIL);
    } else if (got != want) {
        hw_note(it, "duty 回读不符");
        hw_end(it, HW_ST_FAIL);
    } else if (freq < HW_BL_FREQ_HZ - 100 || freq > HW_BL_FREQ_HZ + 100) {
        hw_note(it, "频率偏离 %uHz (期望 %u)", (unsigned)freq, HW_BL_FREQ_HZ);
        hw_end(it, HW_ST_FAIL);
    } else if (!pad_ok) {
        hw_note(it, "背光脚不跟随驱动 (短路/未接通)");
        hw_end(it, HW_ST_FAIL);
    } else {
        hw_end(it, HW_ST_PASS);
    }

    /* ③ SPI2 + ST7789 (无 MISO: 只验写事务) */
    it = hw_begin("lcd.spi", "ST7789 SPI 写通路");
    spi_bus_config_t bus = {
        .mosi_io_num = DISPLAY_MOSI_IO,
        .miso_io_num = GPIO_NUM_NC, /* 本板没接 MISO */
        .sclk_io_num = DISPLAY_SCLK_IO,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * 2 + 64,
        .flags = SPICOMMON_BUSFLAG_MASTER,
    };
    esp_err_t s1 = spi_bus_initialize(DISPLAY_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = DISPLAY_CS_IO,
        .dc_gpio_num = DISPLAY_DC_IO,
        .spi_mode = 0,
        .pclk_hz = DISPLAY_SPI_FREQ_HZ,
        .trans_queue_depth = 4,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_err_t s2 = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)DISPLAY_SPI_HOST, &io_cfg,
                                            &s_io);
    if (s1 != ESP_OK || s2 != ESP_OK) {
        hw_set(it, "spi=%s io=%s", esp_err_to_name(s1), esp_err_to_name(s2));
        hw_note(it, "SPI2 建立失败 (引脚被别的外设占了?)");
        hw_end(it, HW_ST_FAIL);
        return;
    }
    s_done = xSemaphoreCreateBinary();
    esp_lcd_panel_io_callbacks_t cbs = {.on_color_trans_done = lcd_color_done};
    esp_lcd_panel_io_register_event_callbacks(s_io, &cbs, NULL);

    esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = DISPLAY_RST_IO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    esp_err_t s3 = esp_lcd_new_panel_st7789(s_io, &pcfg, &s_panel);
    esp_err_t s4 = ESP_FAIL, s5 = ESP_FAIL, s6 = ESP_FAIL, s7 = ESP_FAIL, s8 = ESP_FAIL;
    if (s3 == ESP_OK) {
        s4 = esp_lcd_panel_reset(s_panel);
        s5 = esp_lcd_panel_init(s_panel);
        uint8_t madctl = DISPLAY_MADCTL;
        s6 = esp_lcd_panel_io_tx_param(s_io, 0x36, &madctl, 1);
        s7 = esp_lcd_panel_set_gap(s_panel, DISPLAY_GAP_X, DISPLAY_GAP_Y);
        uint8_t frctrl2 = 0x00; /* RTNA=0 → ≈119Hz (st7789.c:118) */
        s8 = esp_lcd_panel_io_tx_param(s_io, 0xC6, &frctrl2, 1);
        esp_lcd_panel_invert_color(s_panel, true);
        /* SLPOUT 后要 ~120ms 稳定期 (st7789.h 注释) */
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    hw_set(it, "SPI2@%dMHz pin=%s init=%s", DISPLAY_SPI_FREQ_HZ / 1000000,
           (s1 == ESP_OK) ? "OK" : "FAIL", (s3 == ESP_OK && s5 == ESP_OK) ? "OK" : "FAIL");
    if (s3 != ESP_OK || s4 != ESP_OK || s5 != ESP_OK || s6 != ESP_OK || s7 != ESP_OK ||
        s8 != ESP_OK) {
        hw_note(it, "面板 %s 初始化失败 (MADCTL/gap/FRCTRL2 事务未全部成功)", "ST7789");
        hw_end(it, HW_ST_FAIL);
        return;
    }

    /* 整帧 115200B: 真正跑一次 DMA (不是"参数合法"就算过) */
    void *fb = heap_caps_malloc(DISPLAY_WIDTH * DISPLAY_HEIGHT * 2, MALLOC_CAP_SPIRAM);
    if (!fb) fb = heap_caps_malloc(DISPLAY_WIDTH * DISPLAY_HEIGHT * 2, MALLOC_CAP_DMA);
    if (!fb) {
        hw_note(it, "整帧缓冲分配失败");
        hw_end(it, HW_ST_FAIL);
        return;
    }
    uint16_t *p = (uint16_t *)fb;
    for (int i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++) p[i] = (uint16_t)(i & 0x1F);
    esp_err_t b1 = hw_lcd_blit(fb);
    heap_caps_free(fb);
    if (b1 != ESP_OK) {
        hw_note(it, "整帧 DMA 未完成: %s", esp_err_to_name(b1));
        hw_end(it, HW_ST_FAIL);
        return;
    }
    esp_lcd_panel_disp_on_off(s_panel, true);
    hw_note(it, "无 MISO → 只保证像素已发出, 面板是否收下看末尾看板");
    hw_end(it, HW_ST_PASS);
}

/* ══════════════════════════════════════════════════════════════════════
 * E 段: 触摸
 * ══════════════════════════════════════════════════════════════════════ */
static const touch_pad_t s_pads[TOUCH_CH_COUNT] = {
    TOUCH_PAD_NUM2,  /* 0: GPIO2  — 左侧 */
    TOUCH_PAD_NUM3,  /* 1: GPIO3  — 顶部 0 */
    TOUCH_PAD_NUM4,  /* 2: GPIO4  — 顶部 1 */
    TOUCH_PAD_NUM5,  /* 3: GPIO5  — 顶部 2 */
    TOUCH_PAD_NUM6,  /* 4: GPIO6  — 顶部 3 */
    TOUCH_PAD_NUM7,  /* 5: GPIO7  — 顶部 4 */
    TOUCH_PAD_NUM8,  /* 6: GPIO8  — 右侧 0 */
    TOUCH_PAD_NUM9,  /* 7: GPIO9  — 右侧 1 */
    TOUCH_PAD_NUM10, /* 8: GPIO10 — 右侧 2 */
    TOUCH_PAD_NUM11, /* 9: GPIO11 — 右侧 3 */
    TOUCH_PAD_NUM12, /* 10: GPIO12 — 右侧 4 */
    TOUCH_PAD_NUM13, /* 11: GPIO13 — 右侧 5 */
};

void hw_periph_touch(void)
{
    ESP_LOGI(TAG, "──── E 段: 12 通道触摸 ────");
    hw_item_t *it = hw_begin("touch.init", "触摸外设初始化");

    esp_err_t e1 = touch_pad_init();
    esp_err_t e2 = touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);
    touch_pad_fsm_start();
    int cfg_ok = 0;
    for (int i = 0; i < TOUCH_CH_COUNT; i++) {
        if (touch_pad_config(s_pads[i]) == ESP_OK) cfg_ok++;
        touch_pad_set_thresh(s_pads[i], 800); /* touch_fpc.c:577 */
    }
    hw_set(it, "init=%s fsm=%s 通道 %d/%d", esp_err_to_name(e1), esp_err_to_name(e2), cfg_ok,
           TOUCH_CH_COUNT);
    if (e1 != ESP_OK || e2 != ESP_OK || cfg_ok != TOUCH_CH_COUNT) {
        hw_note(it, "外设初始化/通道配置失败 (GPIO0/1 是 I2C, 不在本段内, 不会互踩)");
        hw_end(it, HW_ST_FAIL);
        return;
    }
    hw_end(it, HW_ST_PASS);

    /* 基线 100×5ms (touch_fpc.c:163-170 同法) */
    uint64_t sum[TOUCH_CH_COUNT] = {0};
    for (int r = 0; r < 100; r++) {
        for (int i = 0; i < TOUCH_CH_COUNT; i++) {
            uint32_t v = 0;
            touch_pad_read_raw_data(s_pads[i], &v);
            sum[i] += v;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    int32_t ref[TOUCH_CH_COUNT];
    for (int i = 0; i < TOUCH_CH_COUNT; i++) ref[i] = (int32_t)(sum[i] / 100);

    /* 抖动: 8 次采样 min/max */
    uint32_t mn[TOUCH_CH_COUNT], mx[TOUCH_CH_COUNT];
    for (int i = 0; i < TOUCH_CH_COUNT; i++) {
        mn[i] = 0xFFFFFFFFu;
        mx[i] = 0;
    }
    for (int s = 0; s < HW_EXP_TOUCH_SAMPLES; s++) {
        for (int i = 0; i < TOUCH_CH_COUNT; i++) {
            uint32_t v = 0;
            touch_pad_read_raw_data(s_pads[i], &v);
            if (v < mn[i]) mn[i] = v;
            if (v > mx[i]) mx[i] = v;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    it = hw_begin("touch.ch", "触摸 12 通道读数");
    int32_t rmin = ref[0], rmax = ref[0], worst = -1;
    int worst_n = 0, dead = 0, sat = 0, flat = 0;
    bool all_same = true;
    for (int i = 0; i < TOUCH_CH_COUNT; i++) {
        if (ref[i] < rmin) rmin = ref[i];
        if (ref[i] > rmax) rmax = ref[i];
        if (ref[i] != ref[0]) all_same = false;
        if (ref[i] < HW_EXP_TOUCH_MIN_RAW) dead++;
        if (ref[i] > HW_EXP_TOUCH_MAX_RAW) sat++;
        int32_t n = (int32_t)(mx[i] - mn[i]);
        if (n > worst) {
            worst = n;
            worst_n = i;
        }
        if (n == 0) flat++;
    }
    hw_set(it, "基线 %ld~%ld 最大抖动 %ld(CH%d), 零抖动 %d 通道", (long)rmin, (long)rmax,
           (long)worst, worst_n, flat);

    if (all_same) {
        hw_note(it, "12 通道基线完全相同 → 触摸外设没在扫描 (raw 常值)");
        hw_end(it, HW_ST_FAIL);
    } else if (sat) {
        hw_note(it, "%d 个通道饱和 (>%d) → 焊盘短到地/进水", sat, HW_EXP_TOUCH_MAX_RAW);
        hw_end(it, HW_ST_FAIL);
    } else if (worst >= 300) {
        /* 抖动 ≥ 按下阈值 (TOP 300/RIGHT 220) = 静置就会自触发 (幻触) */
        hw_note(it, "CH%d 静置抖动 %ld ≥ 按下阈值 → 会幻触", worst_n, (long)worst);
        hw_end(it, HW_ST_FAIL);
    } else if (dead || worst >= 150) {
        /* 恒 0 / 抖动偏大: 面板批次差异下, 曾有通道长期恒 0 而功能未受影响
         * → 只 WARN, 不判死。
         * **零抖动不参与判定**: 8 次采样里安静通道完全可能逐位相同 (量化后无噪声),
         * 拿它判 WARN 会让同一条项 PASS↔WARN 反复横跳 —— 只记进值里给人看。 */
        hw_note(it, "%d 个通道恒 0, 最大抖动 %ld(CH%d) — 需人工看一眼", dead, (long)worst,
                worst_n);
        hw_end(it, HW_ST_WARN);
    } else {
        hw_end(it, HW_ST_PASS);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * G 段: 马达 (EN 全程保持低 → 马达不会动)
 * ══════════════════════════════════════════════════════════════════════ */
void hw_periph_haptic(void)
{
    ESP_LOGI(TAG, "──── G 段: 马达脚 ────");
    /* ① EN 脚: 输入端默认应为低 (板载 10K 下拉 = 上电禁用安全设计) */
    hw_item_t *it = hw_begin("haptic.en", "马达使能脚");
    gpio_config_t ci = {
        .pin_bit_mask = 1ULL << (int)HAPTIC_EN_IO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&ci);
    vTaskDelay(pdMS_TO_TICKS(5));
    int idle = gpio_get_level(HAPTIC_EN_IO);
    char d[40];
    bool fol = pad_follows(HAPTIC_EN_IO, d, sizeof(d));
    gpio_set_level(HAPTIC_EN_IO, 0); /* 恢复禁用 */
    hw_set(it, "悬空=%d (期望 0), 驱动 %s", idle, d);
    if (idle != 0) {
        hw_note(it, "EN 悬空读高 → 板载下拉缺失/EN 被外部拉高 (上电即驱动马达)");
        hw_end(it, HW_ST_FAIL);
    } else if (!fol) {
        hw_note(it, "EN 脚不跟随驱动 (短路/未接通)");
        hw_end(it, HW_ST_FAIL);
    } else {
        hw_end(it, HW_ST_PASS);
    }

    /* ② PWM 脚 + LEDC 通道 (EN 保持低: 只验脚, 马达不动作)
     * 脚自检同样必须在 LEDC 配置之前 (gpio_config 摘路由) */
    it = hw_begin("haptic.pwm", "马达 PWM");
    char p[40];
    bool pfol = pad_follows(HAPTIC_PWM_IO, p, sizeof(p));
    ledc_timer_config_t t = {
        .speed_mode = HW_LEDC_SPD,
        .duty_resolution = HW_HAPTIC_RES,
        .timer_num = HW_HAPTIC_TIMER,
        .freq_hz = HAPTIC_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t e1 = ledc_timer_config(&t);
    ledc_channel_config_t ch = {
        .gpio_num = HAPTIC_PWM_IO,
        .speed_mode = HW_LEDC_SPD,
        .channel = HW_HAPTIC_CH,
        .timer_sel = HW_HAPTIC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .intr_type = LEDC_INTR_DISABLE,
    };
    esp_err_t e2 = ledc_channel_config(&ch);
    uint32_t want = (uint32_t)HW_EXP_HAPTIC_DUTY_PCT * 1023 / 100;
    esp_err_t e3 = ledc_set_duty(HW_LEDC_SPD, HW_HAPTIC_CH, want);
    e3 |= ledc_update_duty(HW_LEDC_SPD, HW_HAPTIC_CH);
    vTaskDelay(pdMS_TO_TICKS(20)); /* duty_rd 下一周期才锁存, 同背光那条 */
    uint32_t got = ledc_get_duty(HW_LEDC_SPD, HW_HAPTIC_CH);
    uint32_t freq = ledc_get_freq(HW_LEDC_SPD, HW_HAPTIC_TIMER);
    /* 收尾: duty 0 + EN 低 (板子交出去时马达必须是停的) */
    ledc_set_duty(HW_LEDC_SPD, HW_HAPTIC_CH, 0);
    ledc_update_duty(HW_LEDC_SPD, HW_HAPTIC_CH);
    gpio_set_level(HAPTIC_EN_IO, 0);
    hw_set(it, "%uHz duty 回读 %u/%u, 脚 %s", (unsigned)freq, (unsigned)got, (unsigned)want, p);
    if (e1 != ESP_OK || e2 != ESP_OK || e3 != ESP_OK) {
        hw_note(it, "LEDC 配置失败");
        hw_end(it, HW_ST_FAIL);
    } else if (got != want) {
        hw_note(it, "duty 回读不符");
        hw_end(it, HW_ST_FAIL);
    } else if (freq < HAPTIC_PWM_FREQ_HZ - 200 || freq > HAPTIC_PWM_FREQ_HZ + 200) {
        hw_note(it, "频率偏离 %uHz (期望 %d — 主工程 board.h 注释 2000Hz 是错的)",
                (unsigned)freq, HAPTIC_PWM_FREQ_HZ);
        hw_end(it, HW_ST_FAIL);
    } else if (!pfol) {
        hw_note(it, "PWM 脚不跟随驱动 (短路/未接通)");
        hw_end(it, HW_ST_FAIL);
    } else {
        hw_end(it, HW_ST_PASS);
    }
}
