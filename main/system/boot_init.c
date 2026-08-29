/**
 * @file boot_init.c
 * @brief 启动初始化序列 (⑤-3 从 main.c 原样搬移, 零逻辑改动)
 *
 * app_main 前的全部初始化编排: flash 写锁 → OTA 回滚确认 → LCD/背光复位 →
 * NVS/重启探针 → SPIFFS → I2C/LVGL → 音频 → 存储挂载 → Touch/传感器 →
 * UI 组装 → power_manager → WiFi → NTP。日志标签保留 "main" (与现状一致,
 * 主循环仍在 main.c)。主循环本体留在 main.c app_main。
 *
 * 屏幕显示电源状态机已并入 power_manager.c (③-4′) — 外部一律经
 * power_manager_screen_state/note_interaction/poll/set_off_timeout_s
 * 交互, main.c 不再持有该域状态。
 * 功耗/睡眠离线诊断 (power_log/power_seg/息屏锁 dump) 已拆出
 * power_diag.c (⑤-1) — 见 system/power_diag.h */
#include "boot_init.h"

#include "board.h"
#include "driver/gpio.h" /* gpio_config_t — 原 main.c 经 gpio_utils.h 间接引入, 拆出后显式 */
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_rom_sys.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "st7789.h"
#include "font_loader.h"
#include "home_screen.h"
#include "loading_screen.h"
#include "input_handler.h"
#include "face_mapper.h"
#include "session_mgr.h"
#include "message_handler.h"
#include "es8311_drv.h"
#include "tm6604.h"
#include "noise_detector.h"
#include "touch_fpc.h"
#include "hdc1080.h"
#include "opt3001.h"
#include "bq27220.h"
#include "mpu6500.h"
#include "dmp_mpu.h"
#include "shake_detector.h"
#include "tap_detector.h"
#include "pat_detector.h"
#include "sensor_logger.h"
#include "life_log.h"
#include "memory_store.h"
#include "diary_mgr.h"
#include "diary_sync.h"
#include "flash_writer_lock.h"
#include "config_mgr.h"
#include "config_keys.h"
#include "power_manager.h"
#include "time_manager.h"
#include "wifi_manager.h"
#include "tts_client.h"
#include "pet_engine.h"

static const char *TAG = "main";

static i2c_master_bus_handle_t s_i2c_bus = NULL;

static esp_err_t i2c_bus_init(void)
{
    ESP_LOGI(TAG, "I2C0 (SCL=GPIO%d, SDA=GPIO%d)", I2C_MASTER_SCL_IO, I2C_MASTER_SDA_IO);
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {.enable_internal_pullup = true},
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c_bus), TAG, "I2C");
    return ESP_OK;
}

i2c_master_bus_handle_t board_get_i2c_bus(void) { return s_i2c_bus; }

void boot_init(void)
{
    ESP_LOGI(TAG, "════════ Virtualpet启动 ════════");

    /* 0z. flash 写互斥锁最先初始化 — 之后所有 esp_flash API (NVS/LittleFS/
     * FATFS/SPIFFS 底层) 统一互斥, 擦除持锁跨 yield (0x101 风暴根治) */
    flash_writer_lock_init();

    /* 0a. OTA 回滚确认 — 若是 OTA 升级后的首次启动, 立即确认固件有效,
     * 否则 bootloader 3 秒后 (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y)
     * 自动回滚旧固件, 升级永远不生效 */
    {
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_ota_img_states_t ota_state;
        if (running && esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
            ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
                ESP_LOGI(TAG, "OTA 固件已确认 — 回滚取消");
            } else {
                ESP_LOGW(TAG, "OTA 固件确认失败");
            }
        }
    }

    /* 0. 尽早硬件复位 LCD + 背光默认下拉 — 清除重启前残留画面 */
    {
        /* 背光引脚内部下拉 — 从硬件上电起就是低电平=屏幕不亮,
         * 无需主动驱动; 后续 LEDC PWM 初始化会接管该引脚 */
        gpio_config_t bl_cfg = {
            .pin_bit_mask = (1ULL << DISPLAY_BL_IO),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&bl_cfg);
        gpio_set_level(DISPLAY_BL_IO, 0);

        /* LCD 硬件复位 */
        gpio_config_t rst_cfg = {
            .pin_bit_mask = (1ULL << DISPLAY_RST_IO),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&rst_cfg);
        gpio_set_level(DISPLAY_RST_IO, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(DISPLAY_RST_IO, 1);
        vTaskDelay(pdMS_TO_TICKS(120)); /* ST7789 要求复位后 ≥120ms 才能接收命令 */
    }

    /* 1. NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 损坏, 擦除重建…");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* ── 诊断探针: 重启计数 + 复位原因 (区分 挂死 vs 复位 vs 日志假死) ──
     * esp_rom_printf 直写 UART0 FIFO, 绕过日志系统/锁 — 日志系统假死时仍会输出 */
    {
        uint32_t boot_cnt = 0;
        nvs_handle_t bh;
        if (nvs_open("diag", NVS_READWRITE, &bh) == ESP_OK) {
            nvs_get_u32(bh, "boot_cnt", &boot_cnt);
            boot_cnt++;
            nvs_set_u32(bh, "boot_cnt", boot_cnt);
            nvs_commit(bh);
            nvs_close(bh);
        }
        esp_rom_printf("DIAG: BOOT#%u reset=%d\n", (unsigned)boot_cnt, (int)esp_reset_reason());
    }
    config_mgr_init();
    /* 自动息屏时长 (设置页可调): off_s = 彻底息屏秒数, 默认 90 */
    power_manager_set_off_timeout_s(config_get_u32(CFG_KEY_OFF_S, 90));
    /* 宠物名/主人称谓预热 — 首次读会 nvs_open 读 flash, 必须在此 (内部栈) 完成;
     * 后续 PSRAM 栈任务 (ws_client 回调) 只做纯 RAM 缓存读 */
    config_get_str(CFG_KEY_PET_NAME, "萝莉丝");
    config_get_str(CFG_KEY_OWNER_NAME, "主人");

    /* 2. SPIFFS (animation assets) */
    esp_vfs_spiffs_conf_t spiffs_cfg = {
        .base_path = "/spiffs",
        .partition_label = "assets",
        .max_files = 8,
        .format_if_mount_failed = true,
    };
    ret = esp_vfs_spiffs_register(&spiffs_cfg);
    if (ret == ESP_OK) {
        size_t total = 0, used = 0;
        esp_spiffs_info("assets", &total, &used);
        ESP_LOGI(TAG, "SPIFFS: %d/%d KB used", (int)(used / 1024), (int)(total / 1024));
    } else {
        ESP_LOGW(TAG, "SPIFFS mount failed (will retry format)");
    }

    /* 3. I2C */
    ESP_ERROR_CHECK(i2c_bus_init());

    /* 3. Display + LVGL + Pet */
    ESP_ERROR_CHECK(st7789_init());
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    /* LVGL 任务栈必须内部 RAM — flash 写期间 cache 冻结 (mem_writer 等
     * 任务写 memory.txt/FatFS/LittleFS), 冻结窗口内访问 PSRAM 栈 → 双异常 */
    lvgl_cfg.task_priority = 4;
    lvgl_cfg.task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT;
    lvgl_port_init(&lvgl_cfg);

    /* LVGL FS 驱动已就绪, 加载 SPIFFS 字体 */
    font_loader_init();
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = st7789_get_panel_io(),
        .panel_handle = st7789_get_panel(),
        /* draw buffer 21 行 (10KB) 半边高 — 内部堆稳态紧张的权衡:
         * 更大缓冲提高渲染原子性但占死内部堆 (U盘 enter/文件槽分配失败
         * 崩溃), 撕裂代价远小于崩溃; 根治 = 内部堆健康化, 不在 buffer
         * 尺寸上赌 */
        .buffer_size = DISPLAY_WIDTH * 21,
        .hres = DISPLAY_WIDTH,
        .vres = DISPLAY_HEIGHT,
        .monochrome = false,
        .rotation = {.swap_xy = true, .mirror_x = false, .mirror_y = true},
        /* 不用 buff_spiram — draw buffer 每帧高频写, flash 写冻结窗口内
         * 写 PSRAM 缓冲 → 双异常; 内部 buffer + DMA */
        .flags = {.buff_dma = true, .swap_bytes = true},
    };
    lvgl_port_add_disp(&disp_cfg);
    /* 探针: LVGL draw buffer 分配后内部堆 */
    ESP_LOGI(TAG, "MEM[2] LVGL 后: SRAM %u KB (最大块 %u KB)",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));

    /* ── 显示加载界面 (DMP 自检等耗时操作期间) ── */
    loading_screen_init();

    /* Loading 就绪 — 开启显示 + 背光 */
    esp_lcd_panel_disp_on_off(st7789_get_panel(), true);
    st7789_backlight_set(80);

    tm6604_init();
    /* Init ES8311 — PA stays off until first playback */
    es8311_drv_cfg_t es_cfg = ES8311_DRV_DEFAULT_CFG();
    es_cfg.sample_rate = 48000;
    es8311_drv_init(&es_cfg);
    /* PA 上电有爆音 (硬件遗留), 开机开启后常开不再关闭 */
    es8311_drv_set_vol(0);
    es8311_drv_pa_set(true);
    noise_detector_init();

    /* 存储挂载 (/cfg LittleFS + /data FatFS + 首启搬移) — 必须先于一切文件读写;
     * life_log 依赖 /data 可用性 (sensor_logger_data_mounted) */
    sensor_logger_init();
    life_log_init();
    memory_store_init();
    diary_mgr_init();
    diary_sync_init();

    tm6604_vibrate(70, 100);
    vTaskDelay(pdMS_TO_TICKS(150));
    tm6604_vibrate(70, 100);
    pet_engine_init();
    tts_client_init();

    /* 4. Touch */
    ESP_ERROR_CHECK(touch_fpc_init());

    /* 5. Sensors */
    hdc1080_init();
    opt3001_init();
    bq27220_init();
    mpu6500_init();
    dmp_mpu_init();
    shake_detector_init();
    tap_detector_init();
    pat_detector_init();

    /* ── 销毁加载界面, 组装真实主页 UI (内部自持 LVGL 锁) ── */
    home_screen_init();

    /* 手势路由 + 页面交互仲裁 (内部注册回调 + 20ms 定时器) —
     * 与主页组装分持锁 (递归锁嵌套安全, 见 loading_screen.c 注释) */
    lvgl_port_lock(0);
    input_handler_init();

    /* 表情出口: 心情/状态变化 → 动画 (仅 idle 时应用) */
    face_mapper_init();

    /* 连续会话模式 (VAD 半双工多轮对话) */
    session_mgr_init();

    /* 协议帧分发: ws_client 只做传输, 帧语义层从这里注册 */
    message_handler_init();
    lvgl_port_unlock();

    /* 电源管理: 轻睡眠使能 (esp_pm_configure) — 在 UI/会话就绪后、WiFi 之前 */
    ESP_ERROR_CHECK(power_manager_init());

    /* 6. WiFi */
    /* NVS/PHY 校准诊断 */
    {
        nvs_stats_t st;
        if (nvs_get_stats(NULL, &st) == ESP_OK) {
            ESP_LOGI(TAG, "NVS: used=%u free=%u total=%u ns=%u",
                     st.used_entries, st.free_entries, st.total_entries,
                     st.namespace_count);
        }
        nvs_handle_t h;
        if (nvs_open("phy", NVS_READONLY, &h) == ESP_OK) {
            uint32_t ver = 0;
            size_t len = 0;
            nvs_get_u32(h, "cal_version", &ver);
            nvs_get_blob(h, "cal_data", NULL, &len);
            ESP_LOGI(TAG, "PHY: cal_version=%lu cal_data=%u B",
                     (unsigned long)ver, (unsigned)len);
            nvs_close(h);
        } else {
            ESP_LOGI(TAG, "PHY: 无 phy 命名空间 (校准数据从未保存)");
        }
    }
    wifi_manager_init();
    /* 探针: WiFi 初始化后内部堆 — 定位启动期消耗大头 */
    ESP_LOGI(TAG, "MEM[1] WiFi init 后: SRAM %u KB (最大块 %u KB)",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    /* NTP 校时: 须在 esp_netif_init (wifi_manager 内部) 之后启动 SNTP;
     * 启动即轮询, WiFi 连上后 60s 内自动同步 */
    ESP_ERROR_CHECK(time_manager_init());
}
