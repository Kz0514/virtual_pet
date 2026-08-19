/**
 * @file sensor_logger.c
 * @brief 传感器日志 — FatFS 存储最近 3 次读数，供 LLM 上下文使用
 */
#include "sensor_logger.h"
#include "tts_client.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "esp_partition.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

static const char *TAG = "sensor_log";
static const char *DATA_MOUNT = "/data";
static const char *LOG_FILE   = "/data/sensors.csv";

static bool s_ready = false;

esp_err_t sensor_logger_init(void) {
    const esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = true,
        .max_files = 8,   /* 噪音CSV/记忆/宠物存档/wifi.json 并发打开, 4 会撞限 */
        .allocation_unit_size = 4096,
    };
    wl_handle_t wl_handle;

    /* 直接挂载 — WL 元数据已持久化, 正常开机无需擦除 */
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(
        DATA_MOUNT, "data", &cfg, &wl_handle);
    if (err != ESP_OK) {
        /* 首次挂载 (分区非全擦除) 或 WL 状态损坏: 擦除后重试一次 —
         * 而非每次开机都擦 (旧行为会丢掉分区内所有持久数据) */
        ESP_LOGW(TAG, "data 分区挂载失败 (%d), 擦除后重试", err);
        const esp_partition_t *part = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "data");
        if (!part) {
            ESP_LOGW(TAG, "未找到 data 分区");
            return err;
        }
        esp_partition_erase_range(part, 0, part->size);
        err = esp_vfs_fat_spiflash_mount_rw_wl(
            DATA_MOUNT, "data", &cfg, &wl_handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "擦除后仍挂载失败: %d", err);
            return err;
        }
        ESP_LOGI(TAG, "data 分区已初始化 (擦除+格式化, %lu bytes)", part->size);
    }
    s_ready = true;

    /* 预创建日志文件 — 创建失败说明 FAT 已损坏 (断电打断写等,
     * 挂载仍会成功所以 format_if_mount_failed 救不了), 擦除重建 */
    FILE *f = fopen(LOG_FILE, "w");
    if (!f) {
        ESP_LOGW(TAG, "FAT 异常 (文件创建失败), 擦除重建 data 分区");
        esp_vfs_fat_spiflash_unmount_rw_wl(DATA_MOUNT, wl_handle);
        const esp_partition_t *part = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "data");
        if (part) {
            esp_partition_erase_range(part, 0, part->size);
        }
        err = esp_vfs_fat_spiflash_mount_rw_wl(
            DATA_MOUNT, "data", &cfg, &wl_handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "重建后仍挂载失败: %d", err);
            s_ready = false;
            return err;
        }
        f = fopen(LOG_FILE, "w");
    }
    if (f) { fclose(f); ESP_LOGI(TAG, "日志文件已创建"); }
    else    { ESP_LOGW(TAG, "预创建日志文件失败"); }
    ESP_LOGI(TAG, "data 分区就绪");
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
    uint32_t t = unix_sec + 8 * 3600;
    int h = (t / 3600) % 24;
    int m = (t / 60) % 60;
    int s = t % 60;
    snprintf(buf, bufsize, "%02d:%02d:%02d", h, m, s);
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
