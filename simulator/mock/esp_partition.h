/** @file mock/esp_partition.h — Mock flash 分区表 (模拟器无分区)
 *
 * pet_avatar.c 的 anim 分区 mmap (零拷贝动画) 是固件功能; 模拟器没有分区,
 * find_first 返回 NULL → 代码自动回落 SPIFFS 文件路径 (与旧动画池一致).
 */
#pragma once
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t esp_partition_type_t;
typedef uint32_t esp_partition_subtype_t;

typedef struct {
    uint32_t address;
    uint32_t size;
    char label[16];
    esp_partition_type_t type;
    esp_partition_subtype_t subtype;
} esp_partition_t;

typedef struct {
    const esp_partition_t *part;
    void *ptr;
    uint32_t size;
} esp_partition_mmap_handle_t;

#define ESP_PARTITION_TYPE_DATA        0x01
#define ESP_PARTITION_SUBTYPE_ANY      0x00
#define ESP_PARTITION_MMAP_DATA        0x00

/* 无分区: 返回 NULL → 调用方走文件回退路径 */
static inline const esp_partition_t *esp_partition_find_first(
    esp_partition_type_t type, esp_partition_subtype_t subtype,
    const char *label)
{
    (void)type; (void)subtype; (void)label;
    return NULL;
}

static inline esp_err_t esp_partition_mmap(const esp_partition_t *partition,
                                           uint32_t offset, uint32_t size,
                                           uint32_t memory, const void **out_ptr,
                                           esp_partition_mmap_handle_t *out_handle)
{
    (void)partition; (void)offset; (void)size; (void)memory;
    (void)out_ptr; (void)out_handle;
    return ESP_ERR_NOT_SUPPORTED;
}

#ifdef __cplusplus
}
#endif
