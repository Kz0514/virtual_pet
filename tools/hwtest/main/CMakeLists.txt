idf_component_register(
    SRCS
        "main.c"
        "hw_expect.c"
        "hw_report.c"
        "hw_screen.c"
        "hw_i2c.c"
        "hw_devices.c"
        "hw_periph.c"
        "hw_storage.c"
        "hw_wifi.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver          # i2c_master / spi_master / ledc / gpio / i2s / touch / tsens
        esp_adc         # B7 SDA 直流体检 (ADC1_CH0 = GPIO1)
        esp_lcd         # ST7789 SPI 面板
        esp_partition
        nvs_flash
        esp_psram
        esp_littlefs    # ← 仓库 vendored (components/esp_littlefs)
        fatfs
        wear_levelling  # fatfs 的 esp_vfs_fat.h 里拉 wear_levelling.h
        esp_timer       # esp_timer_get_time (main/hw_wifi)
        esp_wifi
        esp_netif
        esp_event
        lwip
        spi_flash
)
