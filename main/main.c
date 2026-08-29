/**
 * @file main.c
 * @brief Virtualpet 固件入口 — ESP32-S3
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "board.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_spiffs.h"
#include "esp_pm.h"
#include "soc/usb_serial_jtag_reg.h"
#include "driver/usb_serial_jtag.h" /* usb_serial_jtag_is_connected — IDF 连接监视器 */
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
#include "home_screen.h"
#include "loading_screen.h"
#include "screen_switch.h"
#include "settings_screen.h"
#include "status_bar.h"
#include "chat_bubble.h"
#include "notify_overlay.h"
#include "brightness_bar.h"
#include "tts_client.h"
#include "noise_detector.h"
#include "shake_detector.h"
#include "tap_detector.h"
#include "pat_detector.h"
#include "face_mapper.h"
#include "session_mgr.h"
#include "message_handler.h"
#include "config_mgr.h"
#include "config_keys.h"
#include "input_handler.h"
#include "home_interaction.h"
#include "sensor_logger.h"
#include "memory_store.h"
#include "flash_writer_lock.h"
#include "time_manager.h"
#include "diary_mgr.h"
#include "life_log.h"
#include "diary_sync.h"
#include "usb_storage.h"
#include "power_manager.h"
#include "weather_client.h"
#include <math.h>

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

/* 屏幕显示电源状态机已并入 power_manager.c (③-4′) — 外部一律经
 * power_manager_screen_state/note_interaction/poll/set_off_timeout_s
 * 交互, main.c 不再持有该域状态。 */

/* : WiFi TSF 激活查询 — 声明于 esp_private/wifi.h:524 (预编译库实现)。
 * 息屏后直查: 1 = WiFi skip 回调在阻断轻睡 (见 wifi_manager_detach_light_sleep_skip) */
bool esp_wifi_internal_is_tsf_active(void);

/* : FreeRTOS-Kernel tasks.c 诊断探针 (本地 patch, prvGetExpectedIdleTime
 * 每次评估时更新) — 定位轻睡窗口来源。dexp/ddel 受采样偏差污染 (采样时
 * main 在 ready 列表/未阻塞), 真实窗口来源 = winname/winrem (窗口评估瞬间
 * 延迟列表头部任务名 + 剩余 ticks, 不受采样偏差影响) */
extern volatile uint32_t xDiagExpectedIdle;
extern volatile TickType_t xDiagNextUnblock;
extern volatile TickType_t xDiagTickCount;
extern volatile uint32_t xDiagEvalCnt;
extern volatile uint32_t xDiagRet0Cnt;  /* : 评估 xReturn=0 (有就绪任务, 不睡) 累计 */
extern volatile uint32_t xDiagRet1Cnt;  /* : 评估 xReturn=1 (头部 1 tick 后到期) 累计 */
extern volatile uint32_t xDiagWinCnt;
extern volatile uint32_t xDiagWinCore;
extern volatile TickType_t xDiagWinRemain;
extern volatile char xDiagWinName[16];
extern volatile uint32_t xDiagSleepErr;    /* : esp_light_sleep_start 最后错误码 */
extern volatile uint32_t xDiagSleepErrCnt; /* : 错误累计计数 */

/* /data/power_log.csv — 2s 一条追加 (state: 0=亮 1=变暗 2=息屏), >64KB 重开。
 * fd 路径零分配 — newlib fopen 分配 FILE+锁 (内部 RAM), 耗尽直接 abort */
static void power_log_append(const bq27220_data_t *bat, int state)
{
    int fd = open("/data/power_log.csv", O_CREAT | O_APPEND | O_WRONLY);
    if (fd < 0)
        return;
    if (lseek(fd, 0, SEEK_END) == 0) {
        static const char hdr[] = "ms,mv,ma,soc,state,sleeps,rejects,winus,nxtalm,dexp,ddel,sof,tsf,prob,evalcnt,ret0,ret1,wincnt,wincore,winrem,winname,err,errcnt,wk,ph,tc,d0,d1,d2,d3,d4,d5,d6,d7,d8,d9,d10,d11\n";
        write(fd, hdr, sizeof(hdr) - 1);
    }
    /* 离线诊断列: 电源/睡眠/触摸三类 — sleeps=轻睡评估次数 (enter_cb),
     * rejects=评估未真睡, winus=最后睡眠窗口 µs, sof/tsf/prob 见下,
     * nxtalm=esp_timer 最早可唤醒 alarm 距现在 µs (巨大=窗口来自 tick 列表),
     * dexp/ddel/evalcnt/ret0/ret1/wincnt/wincore/winrem/winname = tasks.c
     * 内核 patch 探针 (xDiag*), 定位窗口来源; err/errcnt=esp_light_sleep_start
     * 错误码/计数。拔电期间串口死, 只能靠 CSV 判轻睡 */
    uint32_t slp = 0, over30 = 0, rej = 0;
    int64_t wmax = 0, wlast = 0;
    power_manager_get_sleep_stats(&slp, &over30, &wmax, &wlast);
    rej = power_manager_get_rejects();
    bool sof = usb_serial_jtag_is_connected();
    bool tsf = esp_wifi_internal_is_tsf_active();
    uint32_t prob = power_manager_get_sleep_probe();
    int64_t nxtalm = esp_timer_get_next_alarm_for_wake_up() - esp_timer_get_time();
    uint32_t dexp = (uint32_t)xDiagExpectedIdle;
    int64_t ddel = (int64_t)(xDiagNextUnblock - xDiagTickCount);
    /* 触摸诊断列: ph=探针去抖命中数, tc=超阈值通道数, d0..d11=
     * 每通道 delta (raw−baseline, 有符号) — 供睡眠供电偏移形态分析 */
    uint32_t tr[12], tb[12];
    touch_get_raw(tr);
    touch_get_baseline(tb);
    int32_t td[12];
    for (int i = 0; i < 12; i++)
        td[i] = (int32_t)tr[i] - (int32_t)tb[i];
    char line[512];
    int ln = snprintf(line, sizeof(line),
                      "%lld,%u,%d,%u,%d,%u,%u,%lld,%lld,%u,%lld,%d,%d,%u,%u,%u,%u,%u,%u,%u,%s,%u,%u,%u,%u,%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                      (long long)(time_manager_is_synced() ? time_manager_get_unix_sec() * 1000LL : 0),
                      bat->voltage_mv, bat->current_ma, bat->soc_pct, state,
                      (unsigned)slp, (unsigned)rej, (long long)wlast,
                      (long long)nxtalm, (unsigned)dexp, (long long)ddel,
                      (int)sof, (int)tsf, (unsigned)prob,
                      (unsigned)xDiagEvalCnt, (unsigned)xDiagRet0Cnt,
                      (unsigned)xDiagRet1Cnt, (unsigned)xDiagWinCnt,
                      (unsigned)xDiagWinCore, (unsigned)xDiagWinRemain,
                      xDiagWinName,
                      (unsigned)xDiagSleepErr, (unsigned)xDiagSleepErrCnt,
                      (unsigned)power_manager_get_wake_cause(),
                      (unsigned)touch_fpc_probe_hits(), (unsigned)touch_fpc_touched_count(),
                      (int)td[0], (int)td[1], (int)td[2], (int)td[3], (int)td[4], (int)td[5],
                      (int)td[6], (int)td[7], (int)td[8], (int)td[9], (int)td[10], (int)td[11]);
    if (ln > 0)
        write(fd, line, (size_t)ln);
    off_t sz = lseek(fd, 0, SEEK_END);
    close(fd);
    if (sz > 64 * 1024)
        remove("/data/power_log.csv");
}

/* ── 分段功耗/唤醒统计: /data/power_seg.csv (256KB 环形 ≈25h 全量保留,
 * 补 power_log.csv 64KB 只留 ~10min 的验收盲区)。两型行:
 *   S 行: 每 60s 聚合 = 段内平均电流/电压/SOC + 亮屏时长 + 唤醒次数
 *   W 行: 每次亮屏翻转瞬间 = 时刻 (unix+uptime) + 来源 + 当时电量
 * 来源 src: 1=探针 2 连击 (假唤醒嫌疑), 0=其他 (左键/摇动/操作)。
 * fd 零分配路径遍历, 禁 fopen (SRAM 紧张 abort) */
#define SEG_MS (60 * 1000 * 1000LL) /* esp_timer 段长 60s */

static int64_t s_seg_start_us = 0;   /* 当前段起点 */
static int64_t s_seg_last_us = 0;    /* 上次 2s tick 时刻 (亮屏时长差) */
static int64_t s_seg_on_us = 0;      /* 段内亮屏累计 µs */
static int32_t s_seg_ma_sum = 0;     /* 段内电流累计 */
static uint32_t s_seg_ma_cnt = 0, s_seg_mv_cnt = 0, s_seg_soc_sum = 0;
static uint32_t s_seg_mv_sum = 0;
static uint32_t s_seg_wake_cnt = 0;  /* 段内唤醒次数 */
static bool s_bat_last_ok = false;   /* W 行电量取最近一次 bq27220 读数 */
static uint16_t s_bat_last_mv = 0;
static int16_t s_bat_last_ma = 0;
static uint8_t s_bat_last_soc = 0;

static void power_seg_write_row(const char *buf, int len)
{
    int fd = open("/data/power_seg.csv", O_CREAT | O_APPEND | O_WRONLY);
    if (fd < 0)
        return;
    if (lseek(fd, 0, SEEK_END) == 0) {
        static const char hdr[] = "type,ts_ms,up_ms,ma,mv,soc,src,wake_cnt,on_ms,seg_ms\n";
        write(fd, hdr, sizeof(hdr) - 1);
    }
    if (len > 0)
        write(fd, buf, (size_t)len);
    off_t sz = lseek(fd, 0, SEEK_END);
    close(fd);
    if (sz > 256 * 1024)
        remove("/data/power_seg.csv");
}

/* 唤醒翻转点调用: 记录 W 行 (主要靠二次分析来源)。
 * ③-4′ 桥: 实现已迁 power_manager (note_interaction 内临时 extern 调用) —
 * 非 static 仅为此跨文件桥; ⑤-1 迁出 power_diag 时随记账函数一并收敛。 */
void power_seg_note_wake_source(uint8_t wake_src)
{
    s_seg_wake_cnt++;
    char line[160];
    int ln = snprintf(line, sizeof(line),
                      "W,%lld,%u,%d,%d,%u,%u,1,0,0\n",
                      (long long)(time_manager_is_synced() ? time_manager_get_unix_sec() * 1000LL : 0),
                      (unsigned)xTaskGetTickCount(),
                      (int)(s_bat_last_ok ? s_bat_last_ma : 0),
                      (int)(s_bat_last_ok ? s_bat_last_mv : 0),
                      (unsigned)(s_bat_last_ok ? s_bat_last_soc : 0),
                      (unsigned)wake_src);
    power_seg_write_row(line, ln);
}

/* 2s 块调用 (have_bat 时): 累积段统计, 每 60s 落 S 行 */
static void power_seg_tick(const bq27220_data_t *bat)
{
    int64_t nowus = esp_timer_get_time();
    s_bat_last_ok = true;
    s_bat_last_mv = (uint16_t)bat->voltage_mv;
    s_bat_last_ma = (int16_t)bat->current_ma;
    s_bat_last_soc = (uint8_t)bat->soc_pct;
    if (s_seg_last_us && s_seg_start_us) {
        if (power_manager_is_screen_on())
            s_seg_on_us += (nowus - s_seg_last_us);
        s_seg_ma_sum += bat->current_ma;
        s_seg_ma_cnt++;
        s_seg_mv_sum += bat->voltage_mv;
        s_seg_mv_cnt++;
        s_seg_soc_sum += bat->soc_pct;
    } else {
        s_seg_start_us = nowus;
        s_seg_ma_sum = s_seg_mv_sum = 0;
        s_seg_ma_cnt = s_seg_mv_cnt = s_seg_soc_sum = 0;
        s_seg_on_us = 0;
        s_seg_wake_cnt = 0;
    }
    s_seg_last_us = nowus;
    if (nowus - s_seg_start_us >= SEG_MS) {
        char line[160];
        int ln = snprintf(line, sizeof(line),
                          "S,%lld,%u,%d,%d,%u,0,%u,%u,%d\n",
                          (long long)(time_manager_is_synced() ? time_manager_get_unix_sec() * 1000LL : 0),
                          (unsigned)xTaskGetTickCount(),
                          s_seg_ma_cnt ? (int)(s_seg_ma_sum / (int32_t)s_seg_ma_cnt) : 0,
                          s_seg_mv_cnt ? (int)(s_seg_mv_sum / s_seg_mv_cnt) : 0,
                          (unsigned)(s_seg_mv_cnt ? (s_seg_soc_sum + s_seg_mv_cnt / 2u) / s_seg_mv_cnt : 0),
                          (unsigned)(s_seg_on_us / 1000), (unsigned)s_seg_wake_cnt,
                          (int)((nowus - s_seg_start_us) / 1000));
        power_seg_write_row(line, ln);
        s_seg_ma_sum = s_seg_mv_sum = 0;
        s_seg_ma_cnt = s_seg_mv_cnt = s_seg_soc_sum = 0;
        s_seg_on_us = 0;
        s_seg_wake_cnt = 0;
        s_seg_start_us = nowus;
    }
}

void app_main(void)
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

    /* ════ 主循环 ════ */
    ESP_LOGI(TAG, "启动完成.");
    uint32_t last_tick = 0;
    uint32_t last_touch_dbg = 0;
    bool registered = false;
    power_manager_note_interaction(); /* 启动: 空闲计时归零 (亮屏亮态下仅重置计时) */

    while (1) {
        /* 手势处理已移入 LVGL 20ms 定时器 (gesture_timer_cb) */

        /* Touch debug: print filtered values — 活动时 200ms 一打, 空闲 5s 一次
         * (减频防 USB-Serial-JTAG TX 压力卡主循环) */
        {
            static uint8_t dbg_idle_cnt = 0;
            uint32_t dbg_period = pdMS_TO_TICKS(1000);
            bool dbg_active = false;
            int f[12];
            touch_get_filtered(f);
            for (int i = 0; i < 12; i++)
                if (f[i] > 60) { dbg_active = true; break; }
            if (dbg_active) {
                dbg_idle_cnt = 0;
                dbg_period = pdMS_TO_TICKS(200); /* 触摸活动期 200ms 一打 */
            } else if (++dbg_idle_cnt < 5)
                dbg_period = pdMS_TO_TICKS(5000);
            if (xTaskGetTickCount() - last_touch_dbg > dbg_period) {
                int t2[12];
                touch_fpc_get_thr(t2);
                last_touch_dbg = xTaskGetTickCount();
                esp_rom_printf("H0:%u\n", (unsigned)(xTaskGetTickCount() * portTICK_PERIOD_MS)); /* 探针: core0 主循环心跳 (绕过日志系统) */
                /* 附右侧 6 通道当前阈值 (T: 列) */
                ESP_LOGI(TAG, "Touch: %d %d %d %d %d %d %d %d %d %d %d %d T:%d %d %d %d %d %d",
                         f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11],
                         t2[6], t2[7], t2[8], t2[9], t2[10], t2[11]);
            bool touched = false, contacted = false;
            for (int i = 0; i < 12; i++) {
                if (f[i] > 100)
                    touched = true; /* 手在感应范围内 */
                if (f[i] > 250)
                    contacted = true; /* 真实接触 (手搭桌边贴设备时读数常驻 100~190, 不算操作) */
            }

            tap_detector_set_touched(touched);
            uint32_t wake_pad = 0;
            if (power_manager_touch_woke(&wake_pad)) {
                /* v2: 硬件触摸唤醒 (睡眠退出回调置位, 主循环消费) —
                 * 轻触/快速点击在 50Hz 扫描停止期间也能可靠唤醒 */
                ESP_LOGI(TAG, "触摸硬件唤醒 (pad %u) — 亮屏", (unsigned)wake_pad);
                power_manager_note_interaction();
            } else if (contacted && power_manager_is_screen_on()) {
                /* 息屏期不直判 contacted — 唤醒统一走动态频率探针
                 * (touch_fpc_sleep_probe: 限速基线 + 抖动自适应阈值 +
                 * 2 连击去抖; 拔电后 raw 持续偏移时此处会重复误报) */
                power_manager_note_interaction();
            } else {
                power_manager_poll(); /* 空闲判定 + 渐变触发 + 息屏推进 (状态机已于 ③-4′ 归位) */
            }
        }
        } /* : 触摸调试块闭合 — 判定/唤醒逻辑保持 1Hz 节拍 */

        if (!registered && wifi_is_connected()) {
            /* 注册失败时不清 registered, 20秒后重试 */
            static uint32_t reg_retry_at = 0;
            uint32_t now2 = xTaskGetTickCount();
            if (now2 < reg_retry_at) { /* wait */
            } else {
                ESP_LOGI(TAG, "WiFi 已连接, 注册设备…");
                if (api_client_init() == ESP_OK) {
                    registered = true;
                    ESP_LOGI(TAG, "设备已认证");
                    ws_client_connect(api_client_get_token());
                    weather_fetch_once();
                    /* OTA 检查 — 与 register 同上下文同步执行 (独立任务曾被调度跳过导致
                     * OTA 永不触发) */
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

        /* USB 连接感知禁睡 — 连接 = USB 主机在通信 (插着线且枚举正常);
         * 息屏轻睡冻结 USJ 时钟 → COM 口消失被误判"卡死"。用官方
         * usb_serial_jtag_is_connected (1ms 粒度 + 3ms 容差 + 粘滞,
         * 与系统 usb_serial_jtag 锁 CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION
         * 状态一致, 零抖动)。U盘模式下 PHY 在 OTG, USJ 恒 0 — 由
         * usb_storage 的 usb 锁覆盖 */
        {
            static bool s_usb_conn = false;
            bool conn = usb_serial_jtag_is_connected();
            if (conn != s_usb_conn) {
                s_usb_conn = conn;
                power_manager_usb_connection(conn);
            }
        }

        /* 息屏期唤醒探针: 扫描在触摸模块独立任务, 由 esp_timer (RTC 闹钟) 按
         * probe_interval 节拍驱动 (20Hz 快探/深闲 2Hz — 独立任务保证节拍
         * 不被主循环拖慢)。本循环只消费唤醒结果: 判定+去抖+免疫窗全在
         * 触摸模块内完成 */
        if (!power_manager_is_screen_on() && touch_fpc_wake_pending()) {
            power_manager_note_probe_wake(); /* 日志 + W 行 src=1 记账 + 唤醒全流程 */
            /* DMP 轮询跟随息屏档 (唤醒=回亮屏 50ms) */
            dmp_mpu_set_off_interval(250);
        }

        /* 每 2 秒推进宠物状态 + 传感器检测 */
        if (now - last_tick > pdMS_TO_TICKS(2000)) {
            last_tick = now;
            ESP_LOGI(TAG, "2s块: 入口"); /* 探针: 确认主循环到达 2s 块 */

            /* 探针: 轻睡窗口 (esp_timer 最早非SKIP alarm 距现在) +
             * vApplicationSleep 回调内统计 (30s 一次, 减日志量) */
            {
                static uint8_t pm_log_cnt = 0;
                bool pm_log_now = (++pm_log_cnt >= 15);
                if (pm_log_now) pm_log_cnt = 0;
                int64_t pm_now = esp_timer_get_time();
                int64_t pm_next = esp_timer_get_next_alarm_for_wake_up();
                if (pm_log_now)
                    ESP_LOGI(TAG, "PM窗: %lld us", (long long)(pm_next - pm_now));

                uint32_t w_total, w_over30;
                int64_t w_max, w_last;
                power_manager_get_sleep_stats(&w_total, &w_over30, &w_max, &w_last);
                ESP_LOGI(TAG, "PM统: cb=%u 超30ms=%u max=%lld last=%lld err=%u(0x%x,%u)",
                         (unsigned)w_total, (unsigned)w_over30,
                         (long long)w_max, (long long)w_last,
                         (unsigned)xDiagSleepErr, (unsigned)xDiagSleepErr,
                         (unsigned)xDiagSleepErrCnt);
            }

            pet_engine_tick(2000);

            hdc1080_data_t env;
            float lux = 0;
            bq27220_data_t bat;
            bool have_env = (hdc1080_read(&env) == ESP_OK);
            bool have_bat = (bq27220_read(&bat) == ESP_OK);
            opt3001_read_lux(&lux);

            /* 🌡/⚡ 每 30s 打一次 (原 2s — USB-JTAG TX 压力卡主循环) */
            static uint8_t env_log_cnt = 0;
            bool env_log_now = (++env_log_cnt >= 15);
            if (env_log_now) env_log_cnt = 0;
            if (have_env && env_log_now)
                ESP_LOGI(TAG, "🌡 %.1f°C %.0f%%", env.temperature, env.humidity);
            if (have_bat) {
                status_bar_set_battery(bat.soc_pct, bat.voltage_mv);
                if (env_log_now)
                    ESP_LOGI(TAG, "⚡ %umV %u%% %dmA",
                             bat.voltage_mv, bat.soc_pct, bat.current_ma);
                power_log_append(&bat, power_manager_screen_state());
                power_seg_tick(&bat); /* power_seg.csv 段统计 */
                /* 充电状态翻转 = USB 拔/插 → 供电链路瞬态 (VBUS 消失/恢复 +
                 * 充电路径切换) 会跳变触摸 raw → 探针假唤醒; 翻转时通知
                 * 探针进 5s 免疫窗。滞回 ±15mA 死区: 插线时 bq27220
                 * 电流在 ±0 抖动, 会导致免疫窗永续 → 息屏唤醒被吞;
                 * 真实拔插电流 ±30mA+ 不受影响。电流≈0 (未稳定) 不参与 */
                {
                    static int8_t s_last_charge = -1;
                    int8_t chg = (bat.current_ma >  15) ? 1
                                 : (bat.current_ma < -15) ? 0
                                                          : -1;
                    if (s_last_charge != -1 && chg != -1 && s_last_charge != chg) {
                        ESP_LOGI(TAG, "充电状态翻转 (%s→%s) — 触摸探针免疫窗 5s",
                                 s_last_charge ? "充电" : "放电",
                                 chg ? "充电" : "放电");
                        touch_fpc_note_usb_event();
                    }
                    if (chg != -1)
                        s_last_charge = chg;
                }
                /* U盘模式下禁睡跟随充电状态 — 持锁条件 = 插线。判据 SOC 优先:
                 * 满电停充期电池放电电流 -30~-60mA, 纯电流阈值会误判拔线
                 * → 插着电脑满电掉盘; 满电 SOC 恒 100% → SOC≥100 恒视为
                 * 插线禁睡, 非满电看电流 (阈值 -5mA)。拔线 (非满电放电)
                 * 30s 宽限后释放锁 → 平时恢复轻睡。读失败不改变状态 */
                usb_storage_set_charging(bat.soc_pct >= 100 ||
                                         bat.current_ma >= -5);
                /* 息屏每 60s 诊断一次: 轻睡计数 + PM 锁列表 */
                if (!power_manager_is_screen_on()) {
                    static uint8_t diag_cnt = 0;
                    if (++diag_cnt >= 7) { /* 息屏 15s 一次诊断 (加密锁采样) */
                        diag_cnt = 0;
                        power_manager_dump_stats();
                        /* 锁/计时器 dump 写进 power_log.csv (# 注释行) —
                         * 拔电期间串口死, 只能靠这里看。fopen 堆守卫:
                         * newlib fopen 分配 FILE+锁, 内部堆耗尽 abort;
                         * 不足 8KB 跳过 (锁列表仍走上面日志) */
                        if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 8192) {
                            ESP_LOGW(TAG, "内部堆不足, 跳过 power_log.csv 锁诊断");
                        } else {
                        FILE *lf = fopen("/data/power_log.csv", "a");
                        if (lf) {
                            fseek(lf, 0, SEEK_END);
                            fprintf(lf, "# locks @ %lld\n",
                                    (long long)(time_manager_is_synced() ? time_manager_get_unix_sec() * 1000LL : 0));
                            esp_pm_dump_locks(lf);
                            /* esp_timer dump — 定位周期 alarm 来源
                             * (任务名直接可见) */
                            fprintf(lf, "# timers @ %lld\n",
                                    (long long)(time_manager_is_synced() ? time_manager_get_unix_sec() * 1000LL : 0));
                            esp_timer_dump(lf);
                            fclose(lf);
                        }
                        } /* else: 内部堆充足才写 CSV 诊断 */
                        /* 任务延迟探针 — vTaskList 列每任务状态 + 剩余 delay tick:
                         * tick 列表高频到期任务会阻轻睡 (prvGetExpectedIdle
                         * Time < 3 → vApplicationSleep 永不调用)。写
                         * /data/tasks.txt 覆盖, U盘拷出 */
                        static char s_tasklist[2048];
                        vTaskList(s_tasklist);
                        int tfd = open("/data/tasks.txt", O_CREAT | O_TRUNC | O_WRONLY);
                        if (tfd >= 0) {
                            write(tfd, s_tasklist, strlen(s_tasklist));
                            close(tfd);
                        }
                    }
                }
            }

            /* 低电量写盘闸: SOC < 阈值 (BATTERY_CRITICAL_THRESHOLD_PCT) 暂停
             * flash 写 — 断电中断写是 data 分区损坏的元凶; 读失败保持
             * fail-open (闸开可写) */
            memory_store_set_writes_safe(!have_bat ||
                                         bat.soc_pct >= BATTERY_CRITICAL_THRESHOLD_PCT);

            /* Log sensor snapshot */
            if (have_env && have_bat && memory_store_writes_safe()) {
                pet_state_t st = pet_engine_get_state();
                sensor_snapshot_t ss = {
                    /* NTP 同步后写真实 Unix 秒, 未同步回退宠物年龄秒 (避免 0 值污染) */
                    .timestamp = time_manager_is_synced() ? time_manager_get_unix_sec() : st.age_seconds,
                    .temperature = env.temperature,
                    .humidity = (uint8_t)env.humidity,
                    .ambient_lux = lux,
                    .battery_mv = bat.voltage_mv,
                    .battery_pct = bat.soc_pct,
                };
                sensor_logger_append(&ss);
            }

            /* 每日时区拉取 (自动模式 + 24h 节流, 内部判网, 失败保持当前) */
            time_manager_daily_tz_tick();

            /* 日记 HTML 同步 (首次连接后 + 每 6h, 内部判条件/节流) */
            diary_sync_tick();

            /* U盘模式 ( 粘滞): 不检测拔线、不自动退出 —
             * 开关是唯一退出途径; 主机弹出后重新武装盘符 (真 U盘行为) */
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
                    ESP_LOGI(TAG, "内存: SRAM空闲 %u KB (最大块 %u KB) | DMA %u KB (最大块 %u KB)"
                                  " | PSRAM空闲 %u KB (最大块 %u KB) | 主任务栈水位 %u B",
                             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) / 1024),
                             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) / 1024),
                             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024),
                             (unsigned)uxTaskGetStackHighWaterMark(NULL));
                    session_mgr_log_stack();
                }
            }

            status_bar_set_wifi(wifi_is_connected(), 0);
        }

        /* 亮屏 100ms 粒度 (轮询/状态机节拍); 息屏跟随触摸探针动态间隔:
         * 快探 50ms / 深闲 500ms — 深闲时睡眠窗口 50ms→500ms (主功耗收益) */
        vTaskDelay(pdMS_TO_TICKS(power_manager_is_screen_on() ? 100 : touch_fpc_probe_interval_ms()));
    }
}
