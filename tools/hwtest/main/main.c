/**
 * @file main.c
 * @brief hwtest 整板硬件自检 —— 流程编排 (A 段系统信息 + 各段调度)
 *
 * 只回答三件事: **初始化成功吗 / 通讯通吗 / 配置对吗**。全自动判定, 无人工项,
 * 不出声, 不出测试图案, 不联网 (WiFi 只做可用性校验)。
 *
 * 段序 (顺序本身是有讲究的, 见 hw_i2c.h 的顺序纪律):
 *   A 系统信息 → B1/B2 空闲电平+位翻转 (纯 GPIO, 必须最先)
 *   → 建总线 → B3 扫描 → C 六器件 → F 音频 → B4/B5 压力 → B6 双向拖动 (必须最后)
 *   → D 面板 → E 触摸 → G 马达 → H 存储 → I WiFi → 屏幕看板 → 串口 JSON
 *
 * 结果: 串口逐项一行 + 末尾 `HWTEST {json}`; run.py 负责解析成表 + 退出码。
 */
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "driver/temperature_sensor.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hw_devices.h"
#include "hw_expect.h"
#include "hw_i2c.h"
#include "hw_periph.h"
#include "hw_report.h"
#include "hw_screen.h"
#include "hw_storage.h"
#include "hw_wifi.h"

static const char *TAG = "hwtest";

/* ══════════════════════════════════════════════════════════════════════
 * A 段: 系统 / flash / PSRAM / 堆 / 温度 / 复位原因 / 分区表
 * ══════════════════════════════════════════════════════════════════════ */
static const char *reset_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "上电";
    case ESP_RST_EXT: return "外部复位脚";
    case ESP_RST_SW: return "软件重启";
    case ESP_RST_PANIC: return "异常/看门狗";
    case ESP_RST_INT_WDT: return "中断看门狗";
    case ESP_RST_TASK_WDT: return "任务看门狗";
    case ESP_RST_WDT: return "其他看门狗";
    case ESP_RST_DEEPSLEEP: return "深睡唤醒";
    case ESP_RST_BROWNOUT: return "欠压复位";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_USB: return "USB 外设";
    case ESP_RST_JTAG: return "JTAG";
    case ESP_RST_EFUSE: return "eFuse 错误";
    case ESP_RST_PWR_GLITCH: return "电源毛刺";
    case ESP_RST_CPU_LOCKUP: return "CPU 死锁";
    default: return "未记录 (USB-JTAG 硬复位常见)";
    }
}

static void sys_chip(void)
{
    hw_item_t *it = hw_begin("sys.chip", "芯片型号/核数");
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    hw_set(it, "%d 核, rev v%d.%d, %dMHz", ci.cores, ci.revision / 100, ci.revision % 100,
           CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    if (ci.cores != HW_EXP_CHIP_CORES) {
        hw_note(it, "核数 %d ≠ 期望 %d", ci.cores, HW_EXP_CHIP_CORES);
        hw_end(it, HW_ST_FAIL);
    } else {
        hw_end(it, HW_ST_PASS);
    }
}

static void sys_flash(void)
{
    hw_item_t *it = hw_begin("sys.flash", "Flash 容量");
    uint32_t sz = 0;
    esp_err_t e = esp_flash_get_size(NULL, &sz);
    hw_set(it, "%u MB", (unsigned)(sz / 1024 / 1024));
    if (e != ESP_OK || sz != HW_EXP_FLASH_BYTES) {
        hw_note(it, "期望 %u MB (读=%s)", (unsigned)(HW_EXP_FLASH_BYTES / 1024 / 1024),
                esp_err_to_name(e));
        hw_end(it, HW_ST_FAIL);
    } else {
        hw_end(it, HW_ST_PASS);
    }
}

static void sys_psram(void)
{
    hw_item_t *it = hw_begin("sys.psram", "PSRAM 容量 + 读写");
    size_t sz = esp_psram_get_size(); /* v5.5 起无参, 直接返回 0 = 没识别 */
    if (sz == 0) {
        hw_set(it, "未识别");
        hw_note(it, "esp_psram_get_size()=0 (配置没开 PSRAM? 或颗粒没起)");
        hw_end(it, HW_ST_FAIL);
        return;
    }
    /* 1MB 写-读校验 (不信任"能分配"就等于"能读写") */
    uint8_t *buf = heap_caps_malloc(HW_EXP_PSRAM_TEST_BYTES, MALLOC_CAP_SPIRAM);
    bool mem_ok = false;
    if (buf) {
        uint32_t r = 0x12345678u;
        for (size_t i = 0; i < HW_EXP_PSRAM_TEST_BYTES; i++) {
            r = r * 1103515245u + 12345u;
            buf[i] = (uint8_t)(r >> 16);
        }
        r = 0x12345678u;
        mem_ok = true;
        for (size_t i = 0; i < HW_EXP_PSRAM_TEST_BYTES; i++) {
            r = r * 1103515245u + 12345u;
            if (buf[i] != (uint8_t)(r >> 16)) {
                mem_ok = false;
                break;
            }
        }
        heap_caps_free(buf);
    }
    hw_set(it, "%u MB, 1MB 写读 %s", (unsigned)(sz / 1024 / 1024), mem_ok ? "OK" : "失败");
    if (sz < HW_EXP_PSRAM_MIN_BYTES || sz > HW_EXP_PSRAM_MAX_BYTES) {
        hw_note(it, "容量 %uB 不在 %u~%uB", (unsigned)sz, HW_EXP_PSRAM_MIN_BYTES,
                HW_EXP_PSRAM_MAX_BYTES);
        hw_end(it, HW_ST_FAIL);
    } else if (!mem_ok) {
        hw_note(it, "分配成功但读写不符 → PSRAM 颗粒/时序有问题");
        hw_end(it, HW_ST_FAIL);
    } else {
        hw_end(it, HW_ST_PASS);
    }
}

static void sys_heap(void)
{
    hw_item_t *it = hw_begin("sys.heap", "内部堆");
    size_t freeb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    hw_set(it, "空闲 %uKB 最大块 %uKB", (unsigned)(freeb / 1024), (unsigned)(big / 1024));
    if (freeb < HW_EXP_HEAP_FREE_MIN) {
        hw_note(it, "空闲低于 %uKB", (unsigned)(HW_EXP_HEAP_FREE_MIN / 1024));
        hw_end(it, HW_ST_FAIL);
    } else if (big < HW_EXP_HEAP_FREE_MIN) {
        hw_note(it, "最大连续块只有 %uKB — 碎片化", (unsigned)(big / 1024));
        hw_end(it, HW_ST_WARN);
    } else {
        hw_end(it, HW_ST_PASS);
    }
}

static void sys_temp(void)
{
    hw_item_t *it = hw_begin("sys.temp", "片内温度");
    temperature_sensor_handle_t ts = NULL;
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t e = temperature_sensor_install(&cfg, &ts);
    if (e == ESP_OK) e = temperature_sensor_enable(ts);
    float c = 0;
    if (e == ESP_OK) e = temperature_sensor_get_celsius(ts, &c);
    if (e == ESP_OK) {
        temperature_sensor_disable(ts);
        temperature_sensor_uninstall(ts);
    }
    if (e != ESP_OK) {
        hw_set(it, "读取失败: %s", esp_err_to_name(e));
        hw_end(it, HW_ST_FAIL);
        return;
    }
    hw_set(it, "%.1f C", c);
    if (c < HW_EXP_TEMP_MIN_C || c > HW_EXP_TEMP_MAX_C) {
        hw_note(it, "超出 %d~%d C", HW_EXP_TEMP_MIN_C, HW_EXP_TEMP_MAX_C);
        hw_end(it, HW_ST_FAIL);
    } else {
        hw_end(it, HW_ST_PASS);
    }
}

static void sys_boot(void)
{
    /* 纯信息项: 复位原因 / 唤醒源 — 判断"上次是不是崩过"很有用。
     * 本板是原生 USB-Serial-JTAG (无 UART 桥), run.py 的 RTS 复位后常报"未记录"(0),
     * 属正常: 只要 up < 5s 就说明这一靴确实是刚起来的。 */
    hw_item_t *it = hw_begin("sys.boot", "复位原因");
    unsigned up = (unsigned)(esp_timer_get_time() / 1000000);
    hw_set(it, "%s (reason=%d)", reset_str(esp_reset_reason()), (int)esp_reset_reason());
    hw_note(it, "本靴已运行 %us", up);
    hw_end(it, HW_ST_PASS);
}

static void sys_parts(void)
{
    hw_item_t *it = hw_begin("sys.parts", "分区表逐项");
    int bad = 0, found = 0;
    char first[80] = "";
    for (int i = 0; i < HW_EXP_PARTS_N; i++) {
        const hw_part_exp_t *x = &HW_EXP_PARTS[i];
        const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_ANY,
                                                           ESP_PARTITION_SUBTYPE_ANY, x->name);
        if (!p) {
            if (!bad) snprintf(first, sizeof(first), "%s 不存在", x->name);
            bad++;
            continue;
        }
        found++;
        if (p->address != x->offset || p->size != x->size) {
            if (!bad)
                snprintf(first, sizeof(first), "%s @0x%05X/%uB ≠ 0x%05X/%uB", x->name,
                         (unsigned)p->address, (unsigned)p->size, (unsigned)x->offset,
                         (unsigned)x->size);
            bad++;
        }
    }
    hw_set(it, "%d/%d 项吻合", found, HW_EXP_PARTS_N);
    if (bad) {
        hw_note(it, "%d 项不符: %s (烧的是本工程 partitions.csv 吗?)", bad, first);
        hw_end(it, HW_ST_FAIL);
    } else {
        hw_end(it, HW_ST_PASS);
    }
}

static void sys_run(void)
{
    ESP_LOGI(TAG, "──── A 段: 系统 ────");
    sys_chip();
    sys_flash();
    sys_psram();
    sys_heap();
    sys_temp();
    sys_boot();
    sys_parts();
}

/* ══════════════════════════════════════════════════════════════════════
 * 编排
 * ══════════════════════════════════════════════════════════════════════ */
void app_main(void)
{
    int64_t t0 = esp_timer_get_time();
    ESP_LOGI(TAG, "════════ hwtest %s (整板硬件自检) ════════", HW_FW_VERSION);

    sys_run();

    /* B1/B2: 纯 GPIO 段 — 必须排在任何外设访问之前 (它们会摘掉 pad 上的外设路由) */
    hw_i2c_levels();
    hw_i2c_bitbang();

    if (hw_i2c_bus_open() != ESP_OK) {
        hw_bad("i2c.bus", "I2C 总线", "建总线失败 → 后面的器件/音频/压力段全部跳过");
    } else {
        hw_i2c_bus_scan();   /* B3 */
        hw_devices_run();    /* C  六器件 */
        hw_audio_run();      /* F  I2S + 真 MCLK 下的 codec 回读 */
        hw_i2c_stress();     /* B4 满速连打 + 间隔 + 交替 */
        hw_i2c_mclk_stress();/* B5 假 MCLK 开关压力 (半死 codec 的照妖镜) */
        hw_i2c_bridge();     /* B6 双向拖动 — 最后跑 */
        hw_i2c_bus_close();
        hw_i2c_sda_probe();  /* B7 SDA 直流体检 (ADC, 分辨"脚坏/被钳/芯片这侧没上拉") */
        /* B8 只在 B7 判出异常时开: 20 秒量线窗口 (芯片钉低 SDA, 人拿表量板) */
        if (hw_i2c_sda_suspect()) hw_i2c_sda_hold();
    }

    hw_periph_lcd();     /* D */
    hw_periph_touch();   /* E */
    hw_periph_haptic();  /* G */
    hw_storage_run();    /* H */
    hw_wifi_run();       /* I */

    hw_screen_dashboard(); /* 看板 (旁证): 要 115KB 内部 RAM, 所以排在 WiFi 拆栈之后 */
    hw_dump_serial();

    ESP_LOGI(TAG, "════════ 结束: 共 %d 项, 用时 %d ms (不重启, 看板留屏) ════════", hw_total(),
             (int)((esp_timer_get_time() - t0) / 1000));
}
