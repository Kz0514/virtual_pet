/**
 * @file hw_storage.h
 * @brief H 段 flash / NVS / 三个文件系统分区 (通讯 + 配置)
 *
 * 安全纪律 (抄主工程 usb_storage.c:30-32 的红线):
 *   **有数据残留就绝不自动格式化**。挂载失败时先看分区是不是全 0xFF (全新空白),
 *   只有全新空白才 f_mkfs/format; 有残留 → FAIL + 交人工。测试文件用独立名字,
 *   写完即删, 不碰 assets 数据。
 */
#ifndef HW_STORAGE_H
#define HW_STORAGE_H

/* H 段: NVS 统计 + /cfg(LittleFS) + /data(FAT) + /assets(LittleFS 只读) */
void hw_storage_run(void);

#endif /* HW_STORAGE_H */
