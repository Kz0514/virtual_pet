/** @file mock/esp_heap_caps.h — Mock PSRAM allocation → plain malloc/free */
#pragma once
#include <stdlib.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MALLOC_CAP_SPIRAM    (1 << 0)
#define MALLOC_CAP_8BIT      (1 << 1)
#define MALLOC_CAP_DMA       (1 << 2)
#define MALLOC_CAP_DEFAULT   (1 << 3)
#define MALLOC_CAP_INTERNAL  (1 << 4) /* 模拟器全部映射到普通堆, 仅作位标志 */

static inline void *heap_caps_malloc(size_t size, uint32_t caps) {
    (void)caps;
    return malloc(size);
}

static inline void heap_caps_free(void *ptr) {
    free(ptr);
}

static inline size_t heap_caps_get_free_size(uint32_t caps) {
    (void)caps;
    return 1024 * 1024; /* 模拟器恒报充足 — 内存余量判断逻辑走"充裕"分支 */
}

#ifdef __cplusplus
}
#endif
