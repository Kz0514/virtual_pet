/**
 * @file gpio_utils.c
 * @brief GPIO 控制工具 — IDF v5.5 标准 API
 *
 * 提供:
 *   - gpio_output_init(): 配置引脚为推挽输出
 *   - gpio_set():        设置输出电平
 *   - gpio_get():        读取输入电平
 *
 * 典型用途: TPA2011D1_EN(GPIO21), TM6604_EN(GPIO38), 屏幕RST(GPIO41) 等
 */
#include "gpio_utils.h"
#include "esp_log.h"

static const char *TAG = "gpio_utils";

esp_err_t gpio_output_init(gpio_num_t pin, uint32_t initial_level)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d 输出配置失败: %s", pin, esp_err_to_name(ret));
        return ret;
    }
    gpio_set_level(pin, initial_level);
    return ESP_OK;
}

esp_err_t gpio_set(gpio_num_t pin, uint32_t level)
{
    return gpio_set_level(pin, level);
}

int gpio_get(gpio_num_t pin)
{
    return gpio_get_level(pin);
}
