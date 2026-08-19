idf_component_register(
    SRCS
        "main.c"
        "voice_chat.c"
        "lvgl_mem.c"
        # System
        "system/gpio_utils.c"
        "system/wifi_manager.c"
        "system/power_manager.c"
        "system/time_manager.c"
        "system/sensor_logger.c"
        "system/memory_store.c"
        # Drivers - Display (lvgl_port provided by esp_lvgl_port component)
        "drivers/display/st7789.c"
        # Drivers - Touch
        "drivers/touch/touch_fpc.c"
        "drivers/touch/gesture_detect.c"
        # Drivers - Audio
        "drivers/audio/es8311_drv.c"
        # Drivers - Sensors
        "drivers/sensors/mpu6500.c"
        "drivers/sensors/dmp/inv_mpu.c"
        "drivers/sensors/dmp/inv_mpu_dmp_motion_driver.c"
        "drivers/sensors/dmp/dmp_mpu.c"
        "drivers/sensors/opt3001.c"
        "drivers/sensors/hdc1080.c"
        "drivers/sensors/bq27220.c"
        "drivers/sensors/qmc6309.c"
        # Drivers - Haptic
        "drivers/haptic/tm6604.c"
        # App
        "app/pet_engine.c"
        "app/noise_detector.c"
        "app/shake_detector.c"
        "app/tap_detector.c"
        "app/pat_detector.c"
        "app/face_mapper.c"
        "app/animation_mgr.c"
        "app/input_handler.c"
        "app/home_interaction.c"
        "app/sensor_fusion.c"
        "app/diary_mgr.c"
        "app/config_mgr.c"
        "app/event_bus.c"
        # Network
        "network/ws_client.c"
        "network/wifi_scanner.c"
        "network/api_client.c"
        "network/ota_client.c"
        "network/asset_client.c"
        "network/bt_pan_manager.c"
        "network/net_selector.c"
        # AI
        "ai/wake_word.c"
        "ai/prompt_builder.c"
        "ai/tts_client.c"
        "ai/session_mgr.c"
        # UI Screens
        "ui/screens/loading_screen.c"
        "ui/screens/home_screen.c"
        "ui/screens/boot_screen.c"
        "ui/screens/settings_screen.c"
        "ui/screens/diary_screen.c"
        "ui/screens/ota_screen.c"
        # UI Fonts (zh.c 已移除, 运行时从 SPIFFS 加载)
        "ui/fonts/font_loader.c"
        # UI Widgets
        "ui/widgets/brightness_bar.c"
        "ui/widgets/pet_avatar.c"
        "ui/widgets/status_bar.c"
        "ui/widgets/chat_bubble.c"
        "ui/widgets/notify_overlay.c"
        "ui/widgets/weather_overlay.c"

    INCLUDE_DIRS
        "."
        "system"
        "drivers/display"
        "drivers/audio"
        "drivers/touch"
        "drivers/sensors"
        "drivers/sensors/dmp"
        "drivers/haptic"
        "app"
        "network"
        "ai"
        "../assets/animation/zhanli"
        "ui/fonts"
        "ui/screens"
        "ui/widgets"

    REQUIRES
        driver
        esp_lcd
        esp_timer
        esp_psram
        spi_flash
        nvs_flash
        esp_wifi
        esp_http_server
        esp_http_client
        esp_https_ota
        app_update
        json
        lvgl
        fatfs
        spiffs
)
