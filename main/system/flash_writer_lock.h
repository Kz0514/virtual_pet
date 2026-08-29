/**
 * @file flash_writer_lock.h
 * @brief 全局 flash 写互斥 + 读优先让位 (链接器 --wrap 拦截 esp_flash API)
 *
 * 由 CMakeLists 的 -Wl,--wrap=esp_flash_{read,write,erase_region} 激活;
 * 本文件只声明 wrap 入口, 调用方无需直接使用。
 */
#pragma once
#include "esp_err.h"
#include "esp_flash.h"

/* 在 app_main 最早处调用 (nvs 初始化前) — 之后所有 flash 访问统一互斥 */
void flash_writer_lock_init(void);

/* ── wrap 入口 (链接器自动拦截, 签名与 esp_flash API 一致) ── */
esp_err_t __wrap_esp_flash_read(esp_flash_t *chip, void *dst,
                                uint32_t src_offset, size_t size);
esp_err_t __wrap_esp_flash_write(esp_flash_t *chip, const void *src,
                                 uint32_t dst_offset, size_t size);
esp_err_t __wrap_esp_flash_erase_region(esp_flash_t *chip,
                                        uint32_t start_addr, uint32_t len);
