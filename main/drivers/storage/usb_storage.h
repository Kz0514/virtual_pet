/** @file usb_storage.h @brief U盘模式 — USB MSC 暴露 /data 分区给电脑直读 */
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/** 启动时安装 tinyusb 设备栈 + MSC 存储, 挂载 /data (需 sensor_logger_init 先建 WL) */
esp_err_t usb_storage_init(void);

/** 是否处于 U盘模式 (USB 主机持有存储) */
bool usb_storage_is_active(void);

/** /data 当前是否挂载 (APP 态) — life_log / diary_sync 的写盘闸门 */
bool usb_storage_data_mounted(void);

/** 进入 U盘模式: 卸载 /data, 块设备暴露给 USB 主机 (需 APP 态) */
esp_err_t usb_storage_enter(void);

/** 退出 U盘模式: 重新挂载 /data */
esp_err_t usb_storage_exit(void);

/** 主循环 2s 节拍: 拔线检测 (曾枚举后断开) + 从未枚举 5min 超时自动退出 */
void usb_storage_tick(void);

/** : 充电状态驱动禁睡 (用户方案) — main 任务电池块每秒调用,
 * charging = 判据 (main.c): soc≥100 (满电停充耗电为负, 电流不可靠) ||
 * current_ma ≥ -5mA (非满电充电中)。charging → U盘模式下持
 * NO_LIGHT_SLEEP 锁; 拔线 (非满电放电) → 30s 宽限后释放 → 恢复轻睡。
 * 开关常开不再永久禁睡。幂等, 跨任务安全 (内部 spinlock)。 */
void usb_storage_set_charging(bool charging);

/** 请求格式化 /data (需 APP 态): 写 NVS 标志 → esp_restart → boot 整区擦除
 * → 组件自动格式化。不在运行时碰 FatFS (NO_MEM + 悬垂崩溃教训) */
esp_err_t usb_storage_request_format(void);

/** 存储容量 (扇区数 / 扇区字节), 用于设置页显示 */
esp_err_t usb_storage_capacity(uint32_t *sector_count, uint32_t *sector_size);

/** 当前 /data 的 FatFS 盘号 (ff_diskio_get_pdrv_wl — 盘号动态分配), -1=不可用 */
int usb_storage_get_drive(void);

/** 数据分区探测 (总/空闲 KB) — 与 repair 重建互斥 (重建中间态会让 f_getfree
 * 返回 FR_NOT_ENABLED=12 误报"-"); 失败返回 false (卷不可用/重建中) */
bool usb_storage_probe_data(uint32_t *total_kb, uint32_t *free_kb);
