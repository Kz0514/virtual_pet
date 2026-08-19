/**
 * @file st7789.c
 * @brief ST7789 240x240 SPI 显示屏硬件驱动
 *
 * 配置: MADCTL=0xA0 (270°CW), 列偏移=80 (320→240), 背光LEDC PWM
 * LVGL 渲染由 esp_lvgl_port 组件管理, 本驱动仅负责硬件初始化
 */
#include "board.h"
#include "st7789.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"

static const char *TAG = "st7789";

static esp_lcd_panel_io_handle_t s_panel_io = NULL;
static esp_lcd_panel_handle_t    s_panel    = NULL;

/* ── 背光 PWM (LEDC, 10bit) ── */
static bool s_backlight_inited = false;

static esp_err_t backlight_init(void)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .duty_resolution  = LEDC_TIMER_10_BIT,
        .timer_num        = LEDC_TIMER_0,
        .freq_hz          = 23814,
        .clk_cfg          = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&ledc_timer), TAG, "LEDC定时器");

    ledc_channel_config_t ledc_ch = {
        .gpio_num       = DISPLAY_BL_IO,
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_CHANNEL_0,
        .timer_sel      = LEDC_TIMER_0,
        .duty           = 0,     /* 初始占空比 0 — 由调用方设置亮度 */
        .hpoint         = 0,
        .intr_type      = LEDC_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ledc_ch), TAG, "LEDC通道");
    s_backlight_inited = true;
    return ESP_OK;
}

esp_err_t st7789_backlight_set(uint8_t percent)
{
    if (!s_backlight_inited) {
        esp_err_t ret = backlight_init();
        if (ret != ESP_OK) return ret;
    }
    if (percent > 100) percent = 100;
    uint32_t duty = (uint32_t)percent * 1023 / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty), TAG, "set_duty");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0), TAG, "update_duty");
    return ESP_OK;
}

/* ── 硬件初始化: SPI → Panel → MADCTL → Gap → 背光 ── */
esp_err_t st7789_init(void)
{
    ESP_LOGI(TAG, "初始化 ST7789 (SPI2, %d MHz)…",
             DISPLAY_SPI_FREQ_HZ / 1000000);

    /* SPI2: MOSI+SCLK, 无 MISO */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = DISPLAY_MOSI_IO,
        .miso_io_num     = GPIO_NUM_NC,
        .sclk_io_num     = DISPLAY_SCLK_IO,
        .quadwp_io_num   = GPIO_NUM_NC,
        .quadhd_io_num   = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * 2 + 10,
        .flags           = SPICOMMON_BUSFLAG_MASTER,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(DISPLAY_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO), TAG, "SPI");

    /* SPI Panel IO: 4线制, 模式0 */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num       = DISPLAY_CS_IO,
        .dc_gpio_num       = DISPLAY_DC_IO,
        .spi_mode          = 0,
        .pclk_hz           = DISPLAY_SPI_FREQ_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)DISPLAY_SPI_HOST,
                        &io_cfg, &s_panel_io), TAG, "Panel IO");

    /* ST7789 Panel: RGB565, RGB顺序 */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num    = DISPLAY_RST_IO,
        .rgb_ele_order     = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel    = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_panel_io, &panel_cfg, &s_panel), TAG, "Panel");
    esp_lcd_panel_reset(s_panel);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "Panel Init");

    /* MADCTL: 270°CW (MV=1, MY=1). 供应商确认的硬件旋转值 */
    uint8_t madctl = 0xA0;
    esp_lcd_panel_io_tx_param(s_panel_io, 0x36, &madctl, 1);
    ESP_LOGI(TAG, "MADCTL=0x%02X (270°CW)", madctl);

    /* 列偏移: ST7789 物理 320 列 → 活动区 240, 偏移 80 */
    esp_lcd_panel_set_gap(s_panel, 80, 0);

    /* FRCTRL2 (C6h): RTNA=0 → ≈119Hz (0x0F=60Hz 默认) */
    uint8_t frctrl2 = 0x00;
    esp_lcd_panel_io_tx_param(s_panel_io, 0xC6, &frctrl2, 1);
    ESP_LOGI(TAG, "FRCTRL2=0x%02X (≈119Hz)", frctrl2);

    esp_lcd_panel_invert_color(s_panel, true);
    /* 背光和 Display ON 推迟到 loading 界面就绪后由 main.c 打开 */

    ESP_LOGI(TAG, "ST7789 硬件就绪 (270°CW, gap=80)");
    return ESP_OK;
}

esp_lcd_panel_handle_t    st7789_get_panel(void)     { return s_panel; }
esp_lcd_panel_io_handle_t st7789_get_panel_io(void) { return s_panel_io; }
