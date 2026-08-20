/**
 * @file sensor_logger.c
 * @brief 存储挂载 (双文件系统) + 传感器日志
 *
 * 存储架构 (v1.0.198 起):
 *   /cfg  — config 分区 (LittleFS, 880KB) — 设备内部运行数据:
 *           wifi.json / memory.txt / pet.json / sensors.csv / noise.csv
 *           (掉电安全, USB 不可见 — 敏感文件不泄漏)
 *   /data — data 分区 (FatFS, 1MB) — 用户可见数据:
 *           diary/ (日记 HTML) + life/ (交互日志) + README.txt — USB 直读
 *
 * /data 的 FAT 挂载权移交 usb_storage (tinyusb MSC 组件统一持有 WL 句柄,
 * U盘模式切换原子完成卸载/重挂); 本模块只做 WL 层初始化。挂载失败
 * **不再自动擦除重建** — 那是用户数据 (日记/日志), 恢复入口 =
 * 设置页"格式化存储" (FAT 级损坏不自动重建, 走受控恢复)。
 */
#include "sensor_logger.h"
#include "usb_storage.h"
#include "tts_client.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "wear_levelling.h"
#include "esp_partition.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "sensor_log";
static const char *CFG_MOUNT = "/cfg";
static const char *LOG_FILE  = "/cfg/sensors.csv";   /* 内部数据 → LittleFS */

static bool s_ready      = false;   /* /cfg 可用 (传感器日志依赖) */

/* /data 可用性由 usb_storage 维护 (挂载/卸载随 U盘模式切换) */
bool sensor_logger_data_mounted(void) { return usb_storage_data_mounted(); }

/* WL 句柄 — usb_storage 持有并负责 /data 的 FAT 挂载 */
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
wl_handle_t sensor_logger_get_wl_handle(void) { return s_wl_handle; }

static esp_err_t mount_cfg(void)
{
    esp_vfs_littlefs_conf_t cfg = {
        .base_path = CFG_MOUNT,
        .partition_label = "config",
        .format_if_mount_failed = true,   /* 首启对旧 FAT 内容自动格式化 */
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "/cfg (LittleFS) 挂载失败: %d — 内部存储不可用", err);
        return err;
    }
    size_t total = 0, used = 0;
    esp_littlefs_info("config", &total, &used);
    ESP_LOGI(TAG, "/cfg 就绪 (LittleFS, %u/%u KB)",
             (unsigned)(used / 1024), (unsigned)(total / 1024));
    return ESP_OK;
}

/* data 分区 WL 层初始化 — FAT 挂载由 usb_storage_init 完成 */
static esp_err_t mount_data(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "data");
    if (!part) {
        ESP_LOGW(TAG, "未找到 data 分区");
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = wl_mount(part, &s_wl_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "data 分区 WL 初始化失败 (%d) — 降级运行, 可经设置页重试", err);
        return err;
    }
    return ESP_OK;
}

esp_err_t sensor_logger_init(void) {
    bool cfg_ok  = (mount_cfg()  == ESP_OK);
    bool data_ok = (mount_data() == ESP_OK);
    /* /data 的 FAT 挂载统一由 usb_storage 管理 (MSC 组件持有 WL 句柄,
     * U盘模式切换原子); 失败 → 降级, 恢复入口 = 设置页"格式化存储" */
    if (data_ok)
        data_ok = (usb_storage_init() == ESP_OK);

    if (cfg_ok) {
        FILE *f = fopen(LOG_FILE, "w");
        if (f) { fclose(f); ESP_LOGI(TAG, "日志文件已创建"); }
        else   { ESP_LOGW(TAG, "预创建日志文件失败"); }
    }

    s_ready = cfg_ok;
    if (!cfg_ok && !data_ok) {
        ESP_LOGE(TAG, "所有存储分区均不可用 — 全功能降级");
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t sensor_logger_append(const sensor_snapshot_t *s) {
    if (!s_ready || !s) return ESP_FAIL;
    /* TTS 播放期间跳过 — flash 写会禁用 cache 冻结双核 100-400ms,
     * 流式播放的浅缓冲会把它暴露成音频卡顿; 下个 2s 节拍再写 */
    if (tts_client_is_playing()) return ESP_FAIL;

    /* Read existing entries */
    sensor_snapshot_t entries[4];
    int count = sensor_logger_get_recent(entries, 3);
    if (count >= 3) {
        entries[0] = entries[1];
        entries[1] = entries[2];
        entries[2] = *s;
    } else {
        entries[count] = *s;
        count++;
    }

    /* Rewrite file (retry once on fd contention) */
    FILE *f = fopen(LOG_FILE, "w");
    if (!f) { vTaskDelay(pdMS_TO_TICKS(50)); f = fopen(LOG_FILE, "w"); }
    if (!f) { ESP_LOGW(TAG, "打开日志失败"); return ESP_FAIL; }
    for (int i = 0; i < count; i++) {
        fprintf(f, "%lu,%.1f,%u,%.0f,%d,%u\n",
                entries[i].timestamp, entries[i].temperature,
                entries[i].humidity, entries[i].ambient_lux,
                entries[i].battery_mv, entries[i].battery_pct);
    }
    fclose(f);
    return ESP_OK;
}

int sensor_logger_get_recent(sensor_snapshot_t *out, int max_count) {
    if (!s_ready || !out || max_count < 1) return 0;

    FILE *f = fopen(LOG_FILE, "r");
    if (!f) return 0;

    char line[128];
    int count = 0;
    while (fgets(line, sizeof(line), f) && count < max_count) {
        sensor_snapshot_t s;
        if (sscanf(line, "%lu,%f,%hhu,%f,%d,%hhu",
                   &s.timestamp, &s.temperature, &s.humidity,
                   &s.ambient_lux, &s.battery_mv, &s.battery_pct) == 6) {
            out[count++] = s;
        }
    }
    fclose(f);
    return count;
}

void sensor_logger_format_time(uint32_t unix_sec, char *buf, int bufsize) {
    time_t t = (time_t)unix_sec;
    struct tm tm;
    localtime_r(&t, &tm);   /* 尊重 TZ (time_manager 设置), 线程安全 */
    strftime(buf, (size_t)bufsize, "%H:%M:%S", &tm);
}

/* Get most recent sensor data as a human-readable string for LLM context */
int sensor_logger_get_context_str(char *buf, int bufsize) {
    if (!s_ready || !buf || bufsize < 32) return 0;
    sensor_snapshot_t s;
    int count = sensor_logger_get_recent(&s, 1);
    if (count < 1) return 0;
    return snprintf(buf, bufsize,
        "温度%.0f°C 湿度%u%% 光照%.0flux 电量%u%%",
        s.temperature, s.humidity, s.ambient_lux, s.battery_pct);
}
