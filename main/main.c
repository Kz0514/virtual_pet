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
#include "loading_screen.h"
#include "screen_switch.h"
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
#include "power_manager.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include <math.h>

static const char *TAG = "main";

static char s_http_buf[1024];
static int s_http_len = 0;

/* ── 主屏幕引用 (用于从子界面恢复) ── */
static lv_obj_t *s_main_scr = NULL;

/* 前向声明 */
void main_screen_note_interaction(void);
void main_restore_home(void)
{
    if (s_main_scr) {
        lvgl_port_lock(0); /* main 线程调 lv_ API 必须持锁 (见 loading_screen.c 注释) */
        screen_load_full(s_main_scr);
        lvgl_port_unlock();
        main_screen_note_interaction();
    }
}

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && s_http_len + evt->data_len < sizeof(s_http_buf) - 1) {
        memcpy(s_http_buf + s_http_len, evt->data, evt->data_len);
        s_http_len += evt->data_len;
        s_http_buf[s_http_len] = '\0';
    }
    return ESP_OK;
}

static void http_get(const char *url)
{
    s_http_len = 0;
    esp_http_client_config_t cfg = {.url = url, .event_handler = http_event_cb, .timeout_ms = 10000};
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    esp_http_client_perform(cli);
    esp_http_client_cleanup(cli);
}

static void fetch_weather_once(void)
{
    const char *token = api_client_get_token();
    char url[384];
    snprintf(url, sizeof(url), "http://%s:%d/api/v1/weather/ip_location?token=%s",
             SERVER_HOST, SERVER_PORT, token);
    http_get(url);
    cJSON *loc = cJSON_Parse(s_http_buf);
    if (!loc)
        return;
    cJSON *ad = cJSON_GetObjectItem(loc, "adcode");
    char adcode_str[16] = "110101";
    if (cJSON_IsString(ad))
        strncpy(adcode_str, ad->valuestring, sizeof(adcode_str) - 1);
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

/* ── 屏幕节能状态机: 亮 → (off_s-30s) 渐变变暗 → 保持暗态 → off_s 渐变息屏 ── */
static bool s_screen_on = true;
static bool s_screen_dim = false;    /* 已处于变暗态 */
static bool s_off_fade_done = false; /* 息屏渐变(目标0)是否已完成 — 修复:
                                      变暗后保持暗态直到 off 超时, 不再无条件息屏 */
static uint32_t s_last_interact = 0;

#define DIM_RATIO_PCT 30 /* 变暗 = 设定亮度的 30% */
#define DIM_CAP_PCT 10   /* 变暗上限 10% */
/* 息屏时序 (设置页可调, NVS "off_s" 持久化, 默认 90s):
 * dim = off−30s (下限 15s) — 与原 60s 变暗 / 90s 息屏一致 */
static uint32_t s_dim_after_ms = 60000; /* 无操作多久 → 开始变暗 */
static uint32_t s_off_after_ms = 90000; /* 无操作多久 → 开始息屏 */
#define FADE_STEP_MS 20                 /* 渐变步进 20ms (50Hz) */
#define FADE_STEPS 100                  /* 总步数 100 → 每阶段 2s, 线性 */

/** 设置自动息屏时长 (设置页调值): off_s = 彻底息屏秒数 */
void main_screen_set_off_timeout_s(uint32_t off_s)
{
    if (off_s < 30)
        off_s = 30;
    uint32_t dim_s = (off_s > 30) ? off_s - 30 : 15;
    s_off_after_ms = off_s * 1000;
    s_dim_after_ms = dim_s * 1000;
    ESP_LOGI(TAG, "息屏时序: %lus 变暗 / %lus 息屏", dim_s, off_s);
}

/* 渐变状态 (LVGL 定时器驱动) */
static bool s_fade_active = false;
static volatile bool s_fade_cancel = false;
static uint8_t s_fade_cur, s_fade_target;
static uint8_t s_fade_steps_left;
static uint8_t s_fade_step_size; /* 线性步进量 (×10 精度) */

/* : WiFi TSF 激活查询 — 声明于 esp_private/wifi.h:524 (预编译库实现)。
 * 息屏后直查: 1 = WiFi skip 回调在阻断轻睡 (见 wifi_manager_detach_light_sleep_skip) */
bool esp_wifi_internal_is_tsf_active(void);

/* : FreeRTOS-Kernel tasks.c 诊断探针 (本地 patch 加入, prvGetExpectedIdleTime
 * 每次评估时更新) — 息屏实测 enter 窗口恒 3000-4000us, 需定位窗口来源:
 * dexp = 评估出的 xExpectedIdleTime (ticks; 注意: 采样时 main 自己就在
 * ready 列表, 全局位图必置位 → dexp 恒 0, 是采样偏差不是真实值)
 * ddel = xNextTaskUnblockTime − xTickCount (阻塞任务最早到期差, 同样受
 * 采样偏差污染 — 采样时 main 未阻塞, 头指向 dmp_bg 等的 50ms 级到期)
 * 真实窗口来源 = winname/winrem: 窗口评估 (dexp 2..6 ticks) 瞬间延迟列表
 * 头部任务名 + 剩余 ticks (不受采样偏差影响, 只在窗口瞬间捕获) */
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

/* 临时电流日志 (2026-08-22 电源管理实测): 2s 一条追加 /data/power_log.csv,
 * 断开串口静置后用 U盘模式拷出看真实耗电 (插串口会充电测不准);
 * state: 0=亮 1=变暗 2=息屏; >64KB 重开; 与 sensor_logger 同写盘模式 */
/* : fopen → open/write — newlib fopen 分配 FILE+锁 (内部 RAM), 锁
 * 分配失败直接 abort (SRAM 7KB 时后台帧加载实测崩 locks.c:77)。本函数
 * 每秒执行, 是最高频 fopen 点; fd 路径零分配 (静态 fd 表) */
static void power_log_append(const bq27220_data_t *bat, int state)
{
    int fd = open("/data/power_log.csv", O_CREAT | O_APPEND | O_WRONLY);
    if (fd < 0)
        return;
    if (lseek(fd, 0, SEEK_END) == 0) {
        static const char hdr[] = "ms,mv,ma,soc,state,sleeps,rejects,winus,nxtalm,dexp,ddel,sof,tsf,prob,evalcnt,ret0,ret1,wincnt,wincore,winrem,winname,err,errcnt,wk,ph,tc,d0,d1,d2,d3,d4,d5,d6,d7,d8,d9,d10,d11\n";
        write(fd, hdr, sizeof(hdr) - 1);
    }
    /* 离线诊断列: sleeps=轻睡评估次数 (enter_cb), rejects=评估了但未真睡,
     * winus=最后睡眠窗口 µs, sof=USJ SOF 原始位 (拔电后恒1=PHY卡死,
     * 恒0=hook 应能判定断开) — 拔电期间串口死, 只能靠 CSV 判轻睡
     * 新增: tsf=WiFi TSF 激活查询 (esp_wifi_internal_is_tsf_active,
     * 1=periph skip 阻断轻睡), prob=vApplicationSleep 调用计数探针
     * 新增: nxtalm=esp_timer 最早可唤醒 alarm 距现在 µs (巨大=无
     * alarm, 窗口来自 FreeRTOS tick 列表 — 实测巨大)
     * 新增: dexp=prvGetExpectedIdleTime 评估值 (ticks, 直接成为
     * 窗口), ddel=xNextTaskUnblockTime − xTickCount (阻塞到期差) —
     * 内核 patch 探针 (tasks.c xDiag*), 定位 3-4ms 窗口来源
     * 新增: evalcnt/wincnt=评估/窗口评估累计, wincore=窗口评估核
     * (bit8=调度器挂起=二次评估), winrem/winname=窗口瞬间延迟列表头部
     * 任务剩余 ticks/名字 = 真正的窗口限制者 (不受采样偏差影响)
     * 新增: ret0/ret1=评估 xReturn=0/1 分类计数 (tasks.c xDiagRet*)
     * — ret0 主导 (~300-700/s, 评估时 ready 非空 = SMP 双核 ready
     * 列表镜像), ret1≈0 (头部 1 tick 后到期几乎没有) */
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
     * 每通道 delta (raw−baseline, 有符号)。息屏自动亮屏根因在"睡眠供电
     * 偏移"的形状 (阶跃/缓升/全局/局部) — 全量逐通道偏差进 CSV,
     * 拔电静置后按 state 列对照分析。 */
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

static void fade_tick(lv_timer_t *t)
{
    if (s_fade_cancel) {
        s_fade_cancel = false;
        s_fade_active = false;
        lv_timer_delete(t);
        return;
    }
    if (s_fade_cur > s_fade_target) {
        uint8_t dec = (s_fade_step_size + 5) / 10; /* 四舍五入 */
        if (dec < 1)
            dec = 1;
        s_fade_cur = (s_fade_cur > s_fade_target + dec)
                         ? s_fade_cur - dec
                         : s_fade_target;
        st7789_backlight_set(s_fade_cur);
    }
    if (--s_fade_steps_left == 0) {
        st7789_backlight_set(s_fade_target);
        s_fade_active = false;
        if (s_fade_target == 0)
            s_off_fade_done = true; /* 仅息屏渐变完成置位 */
    }
}

/** 屏幕是否亮着 — home_interaction gated 查询 (息屏期禁摇动/敲击唤醒:
 * 轻睡周期电气瞬态 → 喇叭"啪" → DMP 振动 → tap/shake 假触发 → 假唤醒) */
bool main_screen_is_on(void) { return s_screen_on; }

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
        s_off_fade_done = false;
        /* WiFi 保持连接态浅睡 (息屏不再 stop) — 唤醒零重连延迟:
         * 语音回执/消息推送不再等 3-5s WiFi 重连 + WS 重挂 */
        power_manager_screen_on(); /* 恢复动画 + LVGL 刷新 + 全屏重绘 */
    } else if (s_screen_dim) {
        ESP_LOGI(TAG, "恢复亮度");
        st7789_backlight_set(brightness_bar_get());
        s_screen_dim = false;
    }
    s_last_interact = xTaskGetTickCount();
}

/* 手势事件路由与主页交互分发已移至 input_handler / home_interaction */

/* 诊断探针 diag_heartbeat_core1 已删 (1.0.250): 内部堆回收 ~3KB。
 * 串口卡死定位改由 H0 (core0 主循环) + 日志存活度判断 */

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
    main_screen_set_off_timeout_s(config_get_u32("off_s", 90));
    /* 宠物名/主人称谓预热 — 首次读会 nvs_open 读 flash, 必须在此 (内部栈) 完成;
     * 后续 PSRAM 栈任务 (ws_client 回调) 只做纯 RAM 缓存读 */
    config_get_str("pet_name", "萝莉丝");
    config_get_str("owner_name", "主人");

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
    /* → 教训: LVGL 任务栈曾改 PSRAM , 实测进设置页
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
        .io_handle = st7789_get_panel_io(),
        .panel_handle = st7789_get_panel(),
        /* 84 行 = 2×行高(42px): 设置页导航总是相邻两行变化, LVGL 会
         * 合并成一个 84 行脏区 → 单次渲染、单个 SPI 突发刷完,
         * 两行同帧原子更新 (分片越多, 与面板异步扫描交叉的撕裂机会越多)
         * 1.0.226: 84 → 42 行 (20KB): U盘模式 enter 需内部堆 (1.0.225
         * 实测运行期内部堆稳态仅 ~19KB, tinyusb install ~13KB 需栈+初始化
         * 空间), 40KB draw buffer 占死内部堆。42 行 = 1 行高, 导航两行分
         * 两次 flush (撕裂轻微, 单缓冲本就逐段扫描); 渲染性能损失可接受
         * 1.0.232: 42 → 21 行 (10KB): 运行期内部堆稳态实测跌至 ~4.6KB
         * (1.0.226 时 ~18.8KB), 已在 49s 瞬态耗尽 (fopen 新 FILE 槽的
         * 互斥信号量分配失败 → lock_init abort → 重启循环, 1.0.232 实测);
         * 21 行 = 半行高, 导航两行分 4 次 flush (撕裂略增), 换 10KB 连续
         * 内部余量保稳定 — 撕裂代价远小于崩溃。残余内存吃紧仍待查
         * v2.18 实测: 21 → 42 行 (20KB) 长跑内部堆跌至 SRAM 1KB/最大块
         * 0KB, TTS 下载速率 96→2 KB/s 卡语音 — 立即回退 21 行。
         * 撕裂/卡顿根治 = 内部堆健康化 (fopen 迁移后稳态待测) + 后续
         * 双缓冲/TE 同步, 不在 buffer 尺寸上赌 */
        .buffer_size = DISPLAY_WIDTH * 21,
        .hres = DISPLAY_WIDTH,
        .vres = DISPLAY_HEIGHT,
        .monochrome = false,
        .rotation = {.swap_xy = true, .mirror_x = false, .mirror_y = true},
        /* : 去掉 buff_spiram — draw buffer 每帧高频写, flash 写
         * 冻结窗口内写 PSRAM 缓冲 → 双异常 (1.0.223 实测进设置页即崩);
         * 40KB 内部堆负担可接受 (boot 后内部堆 ~80KB 空闲) */
        .flags = {.buff_dma = true, .swap_bytes = true},
    };
    lvgl_port_add_disp(&disp_cfg);
    /* 1.0.248 探针: LVGL draw buffer 分配后内部堆 */
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

    /* ── 初始化完成: 销毁加载界面, 创建真实 UI ──
     * 整段持 LVGL 锁: UI 树构建期间若与渲染任务并发, invalidate
     * 撞上 rendering_in_progress 会断言死循环 (递归锁, 嵌套安全) */
    lvgl_port_lock(0);
    loading_screen_destroy();
    pet_avatar_init();
    /* 1.0.248: boot 预加载摸头动画 (内部堆充足期) — 首次摸头零延迟 */
    {
        extern void pet_avatar_preload(void);
        pet_avatar_preload();
    }
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

    /* 电源管理: 轻睡眠使能 (esp_pm_configure) — 在 UI/会话就绪后、WiFi 之前 */
    ESP_ERROR_CHECK(power_manager_init());

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
    /* 1.0.248 探针: WiFi 初始化后内部堆 — 定位启动期消耗大头
     * (实测 1.0.247: boot 105KB → WiFi 后骤降, 60s 内枯竭) */
    ESP_LOGI(TAG, "MEM[1] WiFi init 后: SRAM %u KB (最大块 %u KB)",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
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
    static uint32_t s_last_poll = 0; /* 息屏兜底扫描 10Hz 节拍器 */

    while (1) {
        /* 手势处理已移入 LVGL 20ms 定时器 (gesture_timer_cb) */

        /* Touch debug: print filtered values — 触摸活动时 1Hz, 空闲 5s 一次
         * (1.0.248: 原 1Hz 恒打, 20 分钟 ~1200 行 → USB-Serial-JTAG TX 压力
         * → 主循环卡 1s + 243958 串口通道死 (1.0.246 实测) */
        {
            static uint8_t dbg_idle_cnt = 0;
            uint32_t dbg_period = pdMS_TO_TICKS(1000);
            bool dbg_active = false;
            int f[12];
            touch_get_filtered(f);
            for (int i = 0; i < 12; i++)
                if (f[i] > 60) { dbg_active = true; break; }
            if (!dbg_active && ++dbg_idle_cnt < 5)
                dbg_period = pdMS_TO_TICKS(5000);
            else
                dbg_idle_cnt = 0;
            if (xTaskGetTickCount() - last_touch_dbg > dbg_period) {
                last_touch_dbg = xTaskGetTickCount();
                esp_rom_printf("H0:%u\n", (unsigned)(xTaskGetTickCount() * portTICK_PERIOD_MS)); /* 探针: core0() 心跳 (绕过日志系统) */
                ESP_LOGI(TAG, "Touch: %d %d %d %d %d %d %d %d %d %d %d %d",
                         f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11]);
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
                main_screen_note_interaction();
            } else if (contacted && s_screen_on) {
                /* : 息屏期不再直判 contacted — 唤醒统一走 10Hz 兜底
                 * 扫描 (touch_fpc_sleep_probe: 动态基线 + 3 次去抖)。拔电后
                 * raw 持续偏移时此处每 1s 必中一次, 与 10Hz 块重复且无去抖。 */
                main_screen_note_interaction();
            } else {
                uint32_t idle = xTaskGetTickCount() - s_last_interact;

                /* ── 渐变完成后的状态推进 ──
                 * 只认息屏渐变 (目标 0) 完成标志 — 变暗渐变完成后保持暗态,
                 * 直到 off 超时才彻底息屏 (修复: 曾无条件 2s 后即黑屏) */
                if (s_screen_on && s_screen_dim && s_off_fade_done) {
                    ESP_LOGI(TAG, "息屏完成");
                    st7789_backlight_set(0);
                    gesture_set_screen_on(false);
                    s_screen_on = false;
                    s_screen_dim = false;
                    s_off_fade_done = false;
                    power_manager_screen_off(); /* 停动画 + 停 LVGL 刷新 */

                    /* WiFi 保持连接态浅睡: 不再 stop — 唤醒零重连延迟
                     * (语音回执/消息推送立即可用), 代价 = modem sleep 的
                     * DTIM 周期唤醒电流 (~15-25mA vs 停 WiFi 全停)。
                     * wifi 驱动 skip 回调保留 (TSF active 期间跳过轻睡
                     * 保传输完整, beacon 间隔空闲期轻睡正常进入)。
                     * WS 心跳 (30s ping) 继续维持服务器端连接。 */
                }

                /* ── 触发渐变 ── */
                if (s_screen_on && !s_fade_active) {
                    if (!s_screen_dim && idle > pdMS_TO_TICKS(s_dim_after_ms)) {
                        /* 无操作超时 → 线性渐变变暗 (4s) */
                        uint8_t bri = brightness_bar_get();
                        uint8_t target = (uint8_t)(bri * DIM_RATIO_PCT / 100);
                        if (target > DIM_CAP_PCT)
                            target = DIM_CAP_PCT;
                        if (target < 1)
                            target = 1;
                        s_fade_cur = bri;
                        s_fade_target = target;
                        s_fade_steps_left = FADE_STEPS;
                        s_fade_step_size = (uint8_t)(((uint16_t)(bri - target) * 10) / FADE_STEPS);
                        if (s_fade_step_size < 1)
                            s_fade_step_size = 1;
                        s_fade_cancel = false;
                        s_fade_active = true;
                        s_screen_dim = true; /* 标记进入 dim 态 */
                        lvgl_port_lock(0);   /* main 线程创建 LVGL 定时器须持锁 */
                        lv_timer_t *t = lv_timer_create(fade_tick, FADE_STEP_MS, NULL);
                        if (t)
                            lv_timer_set_repeat_count(t, FADE_STEPS);
                        lvgl_port_unlock();
                        ESP_LOGI(TAG, "变暗渐变 %u→%u%% (%lus 无操作)",
                                 bri, target, s_dim_after_ms / 1000);
                    } else if (s_screen_dim && idle > pdMS_TO_TICKS(s_off_after_ms)) {
                        /* 无操作超时 → 线性渐变息屏 (暗态保持期结束) */
                        s_off_fade_done = false; /* 重开息屏渐变时复位 */
                        uint8_t cur = s_fade_cur;
                        s_fade_cur = cur;
                        s_fade_target = 0;
                        s_fade_steps_left = FADE_STEPS;
                        s_fade_step_size = (uint8_t)(((uint16_t)cur * 10) / FADE_STEPS);
                        if (s_fade_step_size < 1)
                            s_fade_step_size = 1;
                        s_fade_cancel = false;
                        s_fade_active = true;
                        lvgl_port_lock(0); /* main 线程创建 LVGL 定时器须持锁 */
                        lv_timer_t *t = lv_timer_create(fade_tick, FADE_STEP_MS, NULL);
                        if (t)
                            lv_timer_set_repeat_count(t, FADE_STEPS);
                        lvgl_port_unlock();
                        ESP_LOGI(TAG, "息屏渐变 %u→0%% (%lus 无操作)",
                                 cur, s_off_after_ms / 1000);
                    }
                }
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

        /* : USB 连接感知禁睡 — SOF 帧 = USB 主机在通信 (插着线且
         * 枚举正常)。息屏轻睡冻结 USJ 时钟 → 主机侧 COM 口消失 (掉串口
         * 被误判"卡死")。SOF 存在 → 持锁禁睡保活; 拔线 SOF 消失 → 释放
         * 恢复轻睡 (省电场景)。U盘模式下 PHY 在 OTG, USJ 无 SOF, 恒 0 —
         * 由 usb_storage 的 usb 锁覆盖, 不冲突。
         * v2.18 修复抖动: 原实现每 100ms 读 SOF 中断原始位, 而 IDF 连接
         * 监视器 (usb_serial_jtag_sof_tick_hook, 每 tick) 检查后即清除该
         * 位 — 100ms 采样撞 <1ms 置位窗口, 读到的是随机竞争结果 →
         * usbconn 锁 100ms 级来回获取/释放 (实测日志)。改用 IDF 官方
         * usb_serial_jtag_is_connected: 1ms 粒度 + 3ms 容差 + 粘滞,
         * 与系统 usb_serial_jtag 锁 (CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION,
         * 连接恒持) 状态一致, 零抖动。 */
        {
            static bool s_usb_conn = false;
            bool conn = usb_serial_jtag_is_connected();
            if (conn != s_usb_conn) {
                s_usb_conn = conn;
                power_manager_usb_connection(conn);
            }
        }

        /* 息屏期兜底触摸扫描 10Hz : 1Hz/4Hz 实测滑动唤醒仍难 —
         * 滑动轻掠单通道接触 <100ms, delta 峰 250~500, 4Hz 仍可能错过峰值
         * (用户实测 4Hz "还是难")。100ms 粒度: 滑动期间每个通道平均采 1 次,
         * 捕捉概率接近 1, 唤醒响应 ≤100ms。亮屏期 50Hz 定时器在跑, 不需要
         * 兜底。主循环 vTaskDelay 走 TIMG tick, 不产生 esp_timer alarm,
         * 不影响轻睡窗口。 */
        if (!s_screen_on && (now - s_last_poll) > pdMS_TO_TICKS(100)) {
            s_last_poll = now;
            /* : 唤醒判定统一走 touch_fpc_sleep_probe — 复用触摸
             * 模块共享判定 (s_ts.touched), 3 次去抖 + 空间上限 10 通道 */
            if (touch_fpc_sleep_probe()) {
                ESP_LOGI(TAG, "触摸兜底唤醒 (10Hz, 去抖确认)");
                main_screen_note_interaction();
            }
        }

        /* 每 2 秒推进宠物状态 + 传感器检测 */
        if (now - last_tick > pdMS_TO_TICKS(2000)) {
            last_tick = now;
            ESP_LOGI(TAG, "2s块: 入口"); /* 探针: 确认主循环到达 2s 块 */

            /* v2 探针: 轻睡窗口 (esp_timer 最早非SKIP alarm 距现在) +
             * vApplicationSleep 回调内统计 (idle 临界区取样, 纯整数)
             * 1.0.248: 30s 一次 (原 2s 一次, 日志量大) */
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

            /* 1.0.248: 🌡/⚡ 从每 2s 降为每 30s — 原频率日志量大,
             * USB-Serial-JTAG TX 压力 → 主循环卡 + 串口通道死 */
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
                power_log_append(&bat, s_screen_on ? (s_screen_dim ? 1 : 0) : 2);
                /* : 充电状态翻转 = USB 拔/插 → 供电链路瞬态 (VBUS
                 * 消失/恢复 + 充电路径切换) 会跳变触摸 raw → 探针假唤醒
                 * (拔线+关U盘模式实测)。通知探针进 5s 免疫窗。插着 USB
                 * 但电流≈0 (bq27220 未稳定) 时不参与翻转判定 */
                {
                    static int8_t s_last_charge = -1;
                    /* : 滞回 ±15mA — 插着 USB 时 bq27220 电流在 ±0
                     * 抖动 (实测 +10/-13mA) 会频繁翻转 → 触摸探针免疫窗
                     * 每 2-6s 续期 5s → 免疫窗永续 → 息屏触摸唤醒被吞
                     * (1.0.246 实测: 221s 起翻转密集, 免疫窗自 227s
                     * 从未关闭, 触摸 232s-243s 持续按压 11s 无唤醒)。
                     * 死区防抖动; 真实拔插电流 >±30mA (拔线实测
                     * 放电 -30~-60mA) 不受影响 */
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
                /* : U盘模式下禁睡跟随充电状态 (用户方案) — 持锁条件
                 * = 插线。判据 SOC 优先 (2026-08-23 数据实锤): 满电后
                 * 充电芯片停充防浮充, 停充期电池放电电流 -30~-60mA
                 * (CSV 实测), 纯电流阈值必误判拔线 → 插着电脑满电掉盘。
                 * 满电 SOC 恒 100% (用户: "满电绝对是 100%") → SOC≥100
                 * 恒视为插线禁睡; 非满电看电流 (充电中 ma>0, 阈值 -5mA
                 * 余量足)。拔线 (非满电放电) 30s 宽限后释放锁 → 平时
                 * 恢复轻睡。读失败 (have_bat=false) 不改变状态 */
                usb_storage_set_charging(bat.soc_pct >= 100 ||
                                         bat.current_ma >= -5);
                /* 息屏每 60s 诊断一次: 轻睡计数 + PM 锁列表 */
                if (!s_screen_on) {
                    static uint8_t diag_cnt = 0;
                    if (++diag_cnt >= 7) { /* : 60s→15s, 加密 I2C_0 锁采样 */
                        diag_cnt = 0;
                        power_manager_dump_stats();
                        /* : 锁列表也写进 power_log.csv (# 注释行) —
                         * 拔电期间串口死, 锁状态只能靠这里看。
                         * : fopen 堆守卫 — newlib fopen 分配 FILE+锁 (内部
                         * RAM), 内部堆耗尽时 locks.c:77 abort 直接重启
                         * (1.0.244 实测崩点: 息屏诊断 143s 撞上内部堆
                         * 枯竭); 守卫不足则跳过, 锁列表仍经
                         * power_manager_dump_stats 上日志 */
                        if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 8192) {
                            ESP_LOGW(TAG, "内部堆不足, 跳过 power_log.csv 锁诊断");
                        } else {
                        FILE *lf = fopen("/data/power_log.csv", "a");
                        if (lf) {
                            fseek(lf, 0, SEEK_END);
                            fprintf(lf, "# locks @ %lld\n",
                                    (long long)(time_manager_is_synced() ? time_manager_get_unix_sec() * 1000LL : 0));
                            esp_pm_dump_locks(lf);
                            /* : esp_timer dump — 息屏窗口恒 3000us,
                             * 定位 3ms 周期 alarm 来源 (名字直接可见) */
                            fprintf(lf, "# timers @ %lld\n",
                                    (long long)(time_manager_is_synced() ? time_manager_get_unix_sec() * 1000LL : 0));
                            esp_timer_dump(lf);
                            fclose(lf);
                        }
                        } /* else: 内部堆充足才写 CSV 诊断 */
                        /* : 任务延迟探针 — vTaskList 列每任务状态 + 剩余
                         * delay tick。谁在 FreeRTOS tick 列表高频到期 →
                         * prvGetExpectedIdleTime < 3 → vApplicationSleep
                         * 永不调用 (轻睡 100% 不进入)。写 /data/tasks.txt 覆盖,
                         * U盘拷出。 */
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

            /* 低电量写盘闸: SOC<5% 暂停 flash 写 — 断电中断写是
             * data 分区损坏的元凶 (8% 电量时发生过);
             * 2026-08-22: 电压 3.7V → SOC 制 (BATTERY_CRITICAL_THRESHOLD_PCT),
             * 读失败保持 fail-open (闸开可写) */
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

        vTaskDelay(pdMS_TO_TICKS(100)); /* 100ms 粒度 : 息屏兜底扫描 10Hz 节拍 (4Hz 实测唤醒难, 保持) */
    }
}
