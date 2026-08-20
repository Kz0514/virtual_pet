/**
 * @file main.c
 * @brief Virtualpet 固件入口 — ESP32-S3
 */
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "st7789.h"
#include "touch_fpc.h"
#include "gesture_detect.h"
#include "hdc1080.h"
#include "opt3001.h"
#include "bq27220.h"
#include "mpu6500.h"
#include "dmp_mpu.h"
#include "server_config.h"
#include "wifi_manager.h"
#include "api_client.h"
#include "ws_client.h"
#include "ota_client.h"
#include "asset_client.h"
#include "gpio_utils.h"
#include "es8311_drv.h"
#include "tm6604.h"
#include "pet_engine.h"
#include "pet_avatar.h"
#include "font_loader.h"
#include "loading_screen.h"
#include "settings_screen.h"
#include "status_bar.h"
#include "chat_bubble.h"
#include "notify_overlay.h"
#include "brightness_bar.h"
#include "voice_chat.h"
#include "tts_client.h"
#include "noise_detector.h"
#include "shake_detector.h"
#include "tap_detector.h"
#include "pat_detector.h"
#include "face_mapper.h"
#include "session_mgr.h"
#include "config_mgr.h"
#include "input_handler.h"
#include "home_interaction.h"
#include "sensor_logger.h"
#include "memory_store.h"
#include "time_manager.h"
#include "diary_mgr.h"
#include "life_log.h"
#include "diary_sync.h"
#include "usb_storage.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include <math.h>

static const char *TAG = "main";

static char s_http_buf[1024];
static int  s_http_len = 0;

/* ── 主屏幕引用 (用于从子界面恢复) ── */
static lv_obj_t *s_main_scr = NULL;

/* 前向声明 */
void main_screen_note_interaction(void);
void main_restore_home(void)
{
    if (s_main_scr) {
        lvgl_port_lock(0);   /* main 线程调 lv_ API 必须持锁 (见 loading_screen.c 注释) */
        lv_scr_load(s_main_scr);
        lvgl_port_unlock();
        main_screen_note_interaction();
    }
}

static esp_err_t http_event_cb(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA && s_http_len + evt->data_len < sizeof(s_http_buf)-1) {
        memcpy(s_http_buf + s_http_len, evt->data, evt->data_len);
        s_http_len += evt->data_len;
        s_http_buf[s_http_len] = '\0';
    }
    return ESP_OK;
}

static void http_get(const char *url) {
    s_http_len = 0;
    esp_http_client_config_t cfg = { .url = url, .event_handler = http_event_cb, .timeout_ms = 10000 };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    esp_http_client_perform(cli);
    esp_http_client_cleanup(cli);
}

static void fetch_weather_once(void) {
    const char *token = api_client_get_token();
    char url[384];
    snprintf(url, sizeof(url), "http://%s:%d/api/v1/weather/ip_location?token=%s",
             SERVER_HOST, SERVER_PORT, token);
    http_get(url);
    cJSON *loc = cJSON_Parse(s_http_buf);
    if (!loc) return;
    cJSON *ad = cJSON_GetObjectItem(loc, "adcode");
    char adcode_str[16] = "110101";
    if (cJSON_IsString(ad)) strncpy(adcode_str, ad->valuestring, sizeof(adcode_str)-1);
    ESP_LOGI(TAG, "📍 %s (adcode=%s)",
             cJSON_GetObjectItem(loc, "city")->valuestring, adcode_str);
    cJSON_Delete(loc);

    snprintf(url, sizeof(url), "http://%s:%d/api/v1/weather/current?city=%s&token=%s",
             SERVER_HOST, SERVER_PORT, adcode_str, token);
    http_get(url);
    cJSON *w = cJSON_Parse(s_http_buf);
    if (w) {
        ESP_LOGI(TAG, "🌤 %s %s°C 湿度:%s%%",
            cJSON_GetObjectItem(w, "weather")->valuestring,
            cJSON_GetObjectItem(w, "temperature")->valuestring,
            cJSON_GetObjectItem(w, "humidity")->valuestring);
        cJSON_Delete(w);
    }
}

static i2c_master_bus_handle_t s_i2c_bus = NULL;

static esp_err_t i2c_bus_init(void)
{
    ESP_LOGI(TAG, "I2C0 (SCL=GPIO%d, SDA=GPIO%d)", I2C_MASTER_SCL_IO, I2C_MASTER_SDA_IO);
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port     = I2C_MASTER_NUM,
        .sda_io_num   = I2C_MASTER_SDA_IO,
        .scl_io_num   = I2C_MASTER_SCL_IO,
        .clk_source   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = true },
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c_bus), TAG, "I2C");
    return ESP_OK;
}

i2c_master_bus_handle_t board_get_i2c_bus(void) { return s_i2c_bus; }

/* ── 屏幕节能状态机: 亮 → 60s渐变变暗 → 90s渐变息屏 ── */
static bool     s_screen_on  = true;
static bool     s_screen_dim = false;   /* 已处于变暗态 */
static uint32_t s_last_interact = 0;

#define DIM_RATIO_PCT  30     /* 变暗 = 设定亮度的 30% */
#define DIM_CAP_PCT    10     /* 变暗上限 10% */
/* 息屏时序 (设置页可调, NVS "off_s" 持久化, 默认 90s):
 * dim = off−30s (下限 15s) — 与原 60s 变暗 / 90s 息屏一致 */
static uint32_t s_dim_after_ms = 60000;   /* 无操作多久 → 开始变暗 */
static uint32_t s_off_after_ms = 90000;   /* 无操作多久 → 开始息屏 */
#define FADE_STEP_MS   20     /* 渐变步进 20ms (50Hz) */
#define FADE_STEPS     100    /* 总步数 100 → 每阶段 2s, 线性 */

/** 设置自动息屏时长 (设置页调值): off_s = 彻底息屏秒数 */
void main_screen_set_off_timeout_s(uint32_t off_s)
{
    if (off_s < 30) off_s = 30;
    uint32_t dim_s = (off_s > 30) ? off_s - 30 : 15;
    s_off_after_ms = off_s * 1000;
    s_dim_after_ms = dim_s * 1000;
    ESP_LOGI(TAG, "息屏时序: %lus 变暗 / %lus 息屏", dim_s, off_s);
}

/* 渐变状态 (LVGL 定时器驱动) */
static bool        s_fade_active = false;
static volatile bool s_fade_cancel = false;
static uint8_t     s_fade_cur, s_fade_target;
static uint8_t     s_fade_steps_left;
static uint8_t     s_fade_step_size;   /* 线性步进量 (×10 精度) */

static void fade_tick(lv_timer_t *t)
{
    if (s_fade_cancel) {
        s_fade_cancel = false;
        s_fade_active = false;
        lv_timer_delete(t);
        return;
    }
    if (s_fade_cur > s_fade_target) {
        uint8_t dec = (s_fade_step_size + 5) / 10;   /* 四舍五入 */
        if (dec < 1) dec = 1;
        s_fade_cur = (s_fade_cur > s_fade_target + dec)
                   ? s_fade_cur - dec : s_fade_target;
        st7789_backlight_set(s_fade_cur);
    }
    if (--s_fade_steps_left == 0) {
        st7789_backlight_set(s_fade_target);
        s_fade_active = false;
    }
}

/** 有操作: 唤醒/恢复亮度并重置空闲计时 (input_handler/home_interaction 亦调用) */
void main_screen_note_interaction(void)
{
    if (s_fade_active) {
        s_fade_cancel = true;
        s_fade_active = false;
        st7789_backlight_set(brightness_bar_get());
    }
    if (!s_screen_on) {
        ESP_LOGI(TAG, "唤醒屏幕");
        st7789_backlight_set(brightness_bar_get());
        gesture_set_screen_on(true);
        s_screen_on = true;
        s_screen_dim = false;
    } else if (s_screen_dim) {
        ESP_LOGI(TAG, "恢复亮度");
        st7789_backlight_set(brightness_bar_get());
        s_screen_dim = false;
    }
    s_last_interact = xTaskGetTickCount();
}

/* 手势事件路由与主页交互分发已移至 input_handler / home_interaction */

void app_main(void)
{
    ESP_LOGI(TAG, "════════ Virtualpet启动 ════════");

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
        vTaskDelay(pdMS_TO_TICKS(120));   /* ST7789 要求复位后 ≥120ms 才能接收命令 */
    }

    /* 1. NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 损坏, 擦除重建…");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    config_mgr_init();
    /* 自动息屏时长 (设置页可调): off_s = 彻底息屏秒数, 默认 90 */
    main_screen_set_off_timeout_s(config_get_u32("off_s", 90));

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
        ESP_LOGI(TAG, "SPIFFS: %d/%d KB used", (int)(used/1024), (int)(total/1024));
    } else {
        ESP_LOGW(TAG, "SPIFFS mount failed (will retry format)");
    }

    /* 3. I2C */
    ESP_ERROR_CHECK(i2c_bus_init());

    /* 3. Display + LVGL + Pet */
    ESP_ERROR_CHECK(st7789_init());
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    /* 1.0.223→1.0.224 教训: LVGL 任务栈曾改 PSRAM (1.0.223), 实测进设置页
     * 即 Double exception — flash 写期间 cache 冻结 (mem_writer 等任务写
     * memory.txt/FatFS/LittleFS), 冻结窗口内访问 PSRAM 栈 → 双异常。
     * 高频访问内存 (任务栈/draw buffer) 必须内部 RAM; 1.0.223 的内存地图
     * 证明 PSRAM 化 + ALWAYSINTERNAL=4096 后内部堆充裕 (boot 150KB),
     * 回退后仍 ~80KB, 无内存压力 */
    lvgl_cfg.task_priority = 4;
    lvgl_cfg.task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT;
    lvgl_port_init(&lvgl_cfg);

    /* LVGL FS 驱动已就绪, 加载 SPIFFS 字体 */
    font_loader_init();
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle    = st7789_get_panel_io(),
        .panel_handle = st7789_get_panel(),
        /* 84 行 = 2×行高(42px): 设置页导航总是相邻两行变化, LVGL 会
         * 合并成一个 84 行脏区 → 单次渲染、单个 SPI 突发刷完,
         * 两行同帧原子更新 (分片越多, 与面板异步扫描交叉的撕裂机会越多)
         * 1.0.226: 84 → 42 行 (20KB): U盘模式 enter 需内部堆 (1.0.225
         * 实测运行期内部堆稳态仅 ~19KB, tinyusb install ~13KB 需栈+初始化
         * 空间), 40KB draw buffer 占死内部堆。42 行 = 1 行高, 导航两行分
         * 两次 flush (撕裂轻微, 单缓冲本就逐段扫描); 渲染性能损失可接受 */
        .buffer_size  = DISPLAY_WIDTH * 42,
        .hres = DISPLAY_WIDTH, .vres = DISPLAY_HEIGHT,
        .monochrome = false,
        .rotation = { .swap_xy = true, .mirror_x = false, .mirror_y = true },
        /* 1.0.224: 去掉 buff_spiram — draw buffer 每帧高频写, flash 写
         * 冻结窗口内写 PSRAM 缓冲 → 双异常 (1.0.223 实测进设置页即崩);
         * 40KB 内部堆负担可接受 (boot 后内部堆 ~80KB 空闲) */
        .flags    = { .buff_dma = true, .swap_bytes = true },
    };
    lvgl_port_add_disp(&disp_cfg);

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

    /* ── 初始化完成: 销毁加载界面, 创建真实 UI ──
     * 整段持 LVGL 锁: UI 树构建期间若与渲染任务并发, invalidate
     * 撞上 rendering_in_progress 会断言死循环 (递归锁, 嵌套安全) */
    lvgl_port_lock(0);
    loading_screen_destroy();
    pet_avatar_init();
    status_bar_init();
    chat_bubble_init();
    notify_overlay_init();
    brightness_bar_init();

    /* 手势路由 + 页面交互仲裁 (内部注册回调 + 20ms 定时器) */
    input_handler_init();

    /* 表情出口: 心情/状态变化 → 动画 (仅 idle 时应用) */
    face_mapper_init();

    /* 连续会话模式 (VAD 半双工多轮对话) */
    session_mgr_init();
    lvgl_port_unlock();

    /* 6. WiFi */
    /* NVS/PHY 校准诊断 — 排查每次开机 "Saving new calibration data" 循环 */
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
    /* NTP 校时: 须在 esp_netif_init (wifi_manager 内部) 之后启动 SNTP;
     * 启动即轮询, WiFi 连上后 60s 内自动同步 */
    ESP_ERROR_CHECK(time_manager_init());

    /* 保存主屏幕引用 (供设置等子界面返回时恢复) */
    lvgl_port_lock(0);
    s_main_scr = lv_scr_act();
    lvgl_port_unlock();

    /* ════ 主循环 ════ */
    ESP_LOGI(TAG, "启动完成.");
    uint32_t last_tick = 0;
    uint32_t last_touch_dbg = 0;
    bool registered = false;
    s_last_interact = xTaskGetTickCount();

    while (1) {
        /* 手势处理已移入 LVGL 20ms 定时器 (gesture_timer_cb) */

        /* Touch debug: print filtered values every 1s */
        if (xTaskGetTickCount() - last_touch_dbg > pdMS_TO_TICKS(1000)) {
            last_touch_dbg = xTaskGetTickCount();
            int f[12]; touch_get_filtered(f);
            ESP_LOGI(TAG, "Touch: %d %d %d %d %d %d %d %d %d %d %d %d",
                     f[0],f[1],f[2],f[3],f[4],f[5],f[6],f[7],f[8],f[9],f[10],f[11]);
            bool touched = false, contacted = false;
            for (int i = 0; i < 12; i++) {
                if (f[i] > 100) touched = true;    /* 手在感应范围内 */
                if (f[i] > 250) contacted = true;  /* 真实接触 (手搭桌边贴设备时读数常驻 100~190, 不算操作) */
            }

            tap_detector_set_touched(touched);
            if (contacted) {
                main_screen_note_interaction();
            } else {
                uint32_t idle = xTaskGetTickCount() - s_last_interact;

                /* ── 渐变完成后的状态推进 ── */
                if (s_screen_on && s_screen_dim && !s_fade_active) {
                    /* 息屏渐变刚结束 (或已处于 dim 态但渐变已跑完) → 彻底息屏 */
                    ESP_LOGI(TAG, "息屏完成");
                    st7789_backlight_set(0);
                    gesture_set_screen_on(false);
                    s_screen_on = false;
                    s_screen_dim = false;
                }

                /* ── 触发渐变 ── */
                if (s_screen_on && !s_fade_active) {
                    if (!s_screen_dim && idle > pdMS_TO_TICKS(s_dim_after_ms)) {
                        /* 无操作超时 → 线性渐变变暗 (4s) */
                        uint8_t bri = brightness_bar_get();
                        uint8_t target = (uint8_t)(bri * DIM_RATIO_PCT / 100);
                        if (target > DIM_CAP_PCT) target = DIM_CAP_PCT;
                        if (target < 1) target = 1;
                        s_fade_cur    = bri;
                        s_fade_target = target;
                        s_fade_steps_left = FADE_STEPS;
                        s_fade_step_size = (uint8_t)(((uint16_t)(bri - target) * 10) / FADE_STEPS);
                        if (s_fade_step_size < 1) s_fade_step_size = 1;
                        s_fade_cancel = false;
                        s_fade_active = true;
                        s_screen_dim = true;   /* 标记进入 dim 态 */
                        lvgl_port_lock(0);   /* main 线程创建 LVGL 定时器须持锁 */
                        lv_timer_t *t = lv_timer_create(fade_tick, FADE_STEP_MS, NULL);
                        if (t) lv_timer_set_repeat_count(t, FADE_STEPS);
                        lvgl_port_unlock();
                        ESP_LOGI(TAG, "变暗渐变 %u→%u%% (%lus 无操作)",
                                 bri, target, s_dim_after_ms / 1000);
                    } else if (s_screen_dim && idle > pdMS_TO_TICKS(s_off_after_ms)) {
                        /* 无操作超时 → 线性渐变息屏 */
                        uint8_t cur = s_fade_cur;
                        s_fade_cur    = cur;
                        s_fade_target = 0;
                        s_fade_steps_left = FADE_STEPS;
                        s_fade_step_size = (uint8_t)(((uint16_t)cur * 10) / FADE_STEPS);
                        if (s_fade_step_size < 1) s_fade_step_size = 1;
                        s_fade_cancel = false;
                        s_fade_active = true;
                        lvgl_port_lock(0);   /* main 线程创建 LVGL 定时器须持锁 */
                        lv_timer_t *t = lv_timer_create(fade_tick, FADE_STEP_MS, NULL);
                        if (t) lv_timer_set_repeat_count(t, FADE_STEPS);
                        lvgl_port_unlock();
                        ESP_LOGI(TAG, "息屏渐变 %u→0%% (%lus 无操作)",
                                 cur, s_off_after_ms / 1000);
                    }
                }
            }
        }

        if (!registered && wifi_is_connected()) {
            /* 注册失败时不清 registered, 20秒后重试 */
            static uint32_t reg_retry_at = 0;
            uint32_t now2 = xTaskGetTickCount();
            if (now2 < reg_retry_at) { /* wait */ }
            else {
                ESP_LOGI(TAG, "WiFi 已连接, 注册设备…");
                if (api_client_init() == ESP_OK) {
                    registered = true;
                    ESP_LOGI(TAG, "设备已认证");
                    ws_client_connect(api_client_get_token());
                    fetch_weather_once();
                    /* OTA 检查 — 同步执行 (曾用 xTaskCreate 任务, 但任务在部分启动
                     * 场景下从未发出 check 请求, 导致 OTA 永不触发; 改为与
                     * register 同上下文, 行为已被 7 次注册验证可靠) */
                    ota_client_check_sync();
                    /* 栈保持内部 RAM — OTA 写 flash (cache 冻结期 PSRAM 栈会崩) */
                    xTaskCreate(asset_update_task, "asset_up", 8192, NULL, 5, NULL);
                } else {
                    reg_retry_at = now2 + pdMS_TO_TICKS(20000);
                    ESP_LOGW(TAG, "注册失败, 20s 后重试");
                }
            }
        }

        /* WS 断线兜底: 组件内置重连不覆盖任务创建失败 (注册后
         * "Error create websocket task"), 未连接时 30s 周期重建客户端 */
        if (registered && wifi_is_connected() && !ws_client_is_connected()) {
            static uint32_t ws_retry_at = 0;
            uint32_t now3 = xTaskGetTickCount();
            if (now3 >= ws_retry_at) {
                ws_retry_at = now3 + pdMS_TO_TICKS(30000);
                ESP_LOGW(TAG, "WS 未连接 — 重建客户端重试");
                ws_client_connect(api_client_get_token());
            }
        }

        /* 设置页"检查更新": 同步执行 (阻塞期间 LVGL 任务照常渲染进度) */
        ota_client_check_poll();

        /* 摇动/敲击检测分发 (home_interaction: 含震动闸门排空与页面级禁用) */
        home_interaction_poll();

        uint32_t now = xTaskGetTickCount();

        /* 每 2 秒推进宠物状态 + 传感器检测 */
        if (now - last_tick > pdMS_TO_TICKS(2000)) {
            last_tick = now;
            pet_engine_tick(2000);

            hdc1080_data_t env;
            float lux = 0;
            bq27220_data_t bat;
            bool have_env = (hdc1080_read(&env) == ESP_OK);
            bool have_bat = (bq27220_read(&bat) == ESP_OK);
            opt3001_read_lux(&lux);

            if (have_env)
                ESP_LOGI(TAG, "🌡 %.1f°C %.0f%%", env.temperature, env.humidity);
            if (have_bat) {
                status_bar_set_battery(bat.soc_pct, bat.voltage_mv);
                ESP_LOGI(TAG, "⚡ %umV %u%% %dmA",
                         bat.voltage_mv, bat.soc_pct, bat.current_ma);
            }

            /* 低电量写盘闸: <3.7V 暂停 flash 写 — 断电中断写是
             * data 分区损坏的元凶 (8% 电量时发生过) */
            memory_store_set_writes_safe(!have_bat || bat.voltage_mv > 3700);

            /* Log sensor snapshot */
            if (have_env && have_bat && memory_store_writes_safe()) {
                pet_state_t st = pet_engine_get_state();
                sensor_snapshot_t ss = {
                    /* NTP 同步后写真实 Unix 秒, 未同步回退宠物年龄秒 (避免 0 值污染) */
                    .timestamp   = time_manager_is_synced() ?
                                   time_manager_get_unix_sec() : st.age_seconds,
                    .temperature = env.temperature,
                    .humidity    = (uint8_t)env.humidity,
                    .ambient_lux = lux,
                    .battery_mv  = bat.voltage_mv,
                    .battery_pct = bat.soc_pct,
                };
                sensor_logger_append(&ss);
            }

            /* 每日时区拉取 (自动模式 + 24h 节流, 内部判网, 失败保持当前) */
            time_manager_daily_tz_tick();

            /* 日记 HTML 同步 (首次连接后 + 每 6h, 内部判条件/节流) */
            diary_sync_tick();

            /* U盘模式: 拔线检测 + 5min 无枚举超时自动退出 */
            usb_storage_tick();

            /* Upload sensor data to server every 30 seconds */
            {
                static int sensor_upload_cnt = 0;
                if (++sensor_upload_cnt >= 15 && ws_client_is_connected()) {
                    sensor_upload_cnt = 0;
                    char sjson[320];
                    char noise_ctx[128];
                    noise_detector_get_context_str(noise_ctx, sizeof(noise_ctx));
                    snprintf(sjson, sizeof(sjson),
                        "{\"type\":\"sensor_data\",\"data\":{"
                        "\"temp\":%.1f,\"hum\":%.0f,\"light\":%.0f,\"battery\":%d,"
                        "\"noise\":%d,\"noise_ctx\":\"%s\"}}",
                        have_env ? env.temperature : -99.0f,
                        have_env ? env.humidity : 0.0f,
                        lux,
                        have_bat ? bat.soc_pct : -1,
                        noise_detector_get_level(),
                        noise_ctx);
                    ws_client_send_json(sjson);
                }
            }

            /* 噪音平均值写盘 (低电量闸) */
            if (memory_store_writes_safe())
                noise_detector_write_csv();

            /* 记忆冲刷 + 元数据缓存刷新 (TTS 空闲时落盘) */
            memory_store_tick();

            /* 内存探查 (30s): SRAM/PSRAM 空闲 + 最大连续块 + 主任务栈水位 */
            {
                static int mem_probe_cnt = 0;
                if (++mem_probe_cnt >= 15) {
                    mem_probe_cnt = 0;
                    ESP_LOGI(TAG, "内存: SRAM空闲 %u KB (最大块 %u KB) | PSRAM空闲 %u KB "
                             "(最大块 %u KB) | 主任务栈水位 %u B",
                             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024),
                             (unsigned)uxTaskGetStackHighWaterMark(NULL));
                    session_mgr_log_stack();
                }
            }

            status_bar_set_wifi(wifi_is_connected(), 0);
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
