/**
 * @file main.c
 * @brief Virtualpet 固件入口 — ESP32-S3
 *
 * 编排壳: boot_init() 完成全部初始化 (system/boot_init.c), 本文件只剩
 * 主循环 (运行时调度)。行数目标 ≤300 — 主循环本身 ~270 行内聚函数,
 * 不再外拆 (纯物理搬移无收益)。
 */
#include <stdio.h>
#include "board.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_heap_caps.h"
#include "driver/usb_serial_jtag.h" /* usb_serial_jtag_is_connected — IDF 连接监视器 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "boot_init.h"
#include "touch_fpc.h"
#include "tap_detector.h"
#include "power_manager.h"
#include "power_diag.h"
#include "api_client.h"
#include "ws_client.h"
#include "ota_client.h"
#include "asset_client.h"
#include "wifi_manager.h"
#include "weather_client.h"
#include "home_interaction.h"
#include "dmp_mpu.h"
#include "pet_engine.h"
#include "hdc1080.h"
#include "opt3001.h"
#include "bq27220.h"
#include "usb_storage.h"
#include "sensor_logger.h"
#include "memory_store.h"
#include "time_manager.h"
#include "diary_sync.h"
#include "noise_detector.h"
#include "status_bar.h"
#include "session_mgr.h"

static const char *TAG = "main";

void app_main(void)
{
    boot_init();

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

            /* 探针: 轻睡窗口 + 统计 (30s 一次, 减日志量) */
            power_diag_pm_stats_log();

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
                power_diag_log_append(&bat, power_manager_screen_state());
                power_diag_seg_tick(&bat); /* power_seg.csv 段统计 */
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
                /* 息屏诊断 (轻睡计数 + 锁/timer dump + tasks.txt, 15s 一次) */
                if (!power_manager_is_screen_on())
                    power_diag_screen_off_diag();
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
