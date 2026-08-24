/**
 * @file mock_impl.c
 * @brief 模拟器硬件桩实现 — 供"extern 声明"方式的调用链接
 *
 * 部分 UI 文件 (如 pet_avatar.c) 用 extern 声明直接引用硬件函数,
 * 不包含头文件, 因此需要真实的链接符号 (static inline 头文件无效).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_app_desc.h"
#include "config_mgr.h"
#include "time_manager.h"
#include "ota_client.h"
#include "usb_storage.h"

/* ── TTS ── */
void tts_client_init(void) {}
bool tts_speak(const char *text) { (void)text; return false; }
bool tts_speak_queue(const char *text) { (void)text; return false; }
bool tts_client_queue_empty(void) { return true; }
bool tts_speak_inst(const char *text, const char *instruction) {
    (void)text; (void)instruction; return false;
}
bool tts_client_is_playing(void) { return false; }
bool tts_client_is_downloading(void) { return false; }
uint32_t tts_client_get_playback_ms(void) { return 0; }
bool tts_client_is_busy(void) { return false; }

/* ── 触觉马达 ── */
void tm6604_vibrate(uint8_t duty_pct, uint16_t duration_ms) {
    (void)duty_pct; (void)duration_ms;
}

/* ── 手势检测 ── */
void tap_detector_suppress(bool on) { (void)on; }
void shake_detector_suppress(bool on) { (void)on; }

/* ── 设置页"息屏时长"回调 (main.c 真机的实时生效钩子) ── */
void main_screen_set_off_timeout_s(uint32_t off_s) { (void)off_s; }

/* ── 设置页"重启设备"→ 模拟器退出 ── */
void esp_restart(void) {
    printf("[sim] esp_restart() — 模拟器退出\n");
    exit(0);
}

/* ── 关于页版本号 ── */
const esp_app_desc_t *esp_app_get_description(void) {
    static const esp_app_desc_t s_desc = {
        .version = "SIM (模拟器)",
        .project_name = "Virtualpet",
        .idf_ver = "PC",
    };
    return &s_desc;
}

/* ── 设置持久化: 进程内键值表 (模拟 NVS) ── */
#define CFG_MAX_KEYS 16
typedef struct { const char *key; uint32_t val; } cfg_entry_t;
static cfg_entry_t s_cfg[CFG_MAX_KEYS];
static int s_cfg_count = 0;

uint32_t config_get_u32(const char *key, uint32_t def) {
    for (int i = 0; i < s_cfg_count; i++) {
        if (strcmp(s_cfg[i].key, key) == 0) return s_cfg[i].val;
    }
    /* 未命中: 返回默认值并缓存 (模拟 NVS 首次读取后驻留) */
    if (s_cfg_count < CFG_MAX_KEYS) {
        s_cfg[s_cfg_count].key = key;   /* 键为字符串字面量, 进程生命期有效 */
        s_cfg[s_cfg_count].val = def;
        s_cfg_count++;
    }
    return def;
}

void config_set_u32(const char *key, uint32_t val) {
    for (int i = 0; i < s_cfg_count; i++) {
        if (strcmp(s_cfg[i].key, key) == 0) {
            s_cfg[i].val = val;
            return;
        }
    }
    if (s_cfg_count < CFG_MAX_KEYS) {
        s_cfg[s_cfg_count].key = key;
        s_cfg[s_cfg_count].val = val;
        s_cfg_count++;
    }
}

/* ── 时区: 设置页调值实时生效钩子, PC 无实际意义 ── */
void time_manager_apply_tz(int32_t offset_sec) {
    printf("[sim] time_manager_apply_tz(%d)\n", (int)offset_sec);
}

/* ── OTA 检查: 无服务器可查, no-op ── */
void ota_client_request_check(void) {
    printf("[sim] ota_client_request_check() — 模拟器无 OTA\n");
}

/* ── U盘模式: 硬件功能, 全部桩 ── */
esp_err_t usb_storage_init(void) { return ESP_OK; }
bool usb_storage_is_active(void) { return false; }
bool usb_storage_data_mounted(void) { return false; }
esp_err_t usb_storage_enter(void) {
    printf("[sim] usb_storage_enter() — 模拟器无 U盘模式\n");
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t usb_storage_exit(void) { return ESP_OK; }
void usb_storage_tick(void) {}
void usb_storage_set_charging(bool charging) { (void)charging; }
esp_err_t usb_storage_request_format(void) {
    printf("[sim] usb_storage_request_format() — 模拟器无格式化\n");
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t usb_storage_capacity(uint32_t *sector_count, uint32_t *sector_size) {
    (void)sector_count; (void)sector_size;
    return ESP_FAIL;
}
int usb_storage_get_drive(void) { return -1; }
bool usb_storage_probe_data(uint32_t *total_kb, uint32_t *free_kb) {
    (void)total_kb; (void)free_kb;
    return false;
}
