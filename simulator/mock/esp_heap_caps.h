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

static inline void *heap_caps_malloc(size_t size, uint32_t caps) {
    (void)caps;
    return malloc(size);
}

static inline void heap_caps_free(void *ptr) {
    free(ptr);
}

#ifdef __cplusplus
}
#endif
