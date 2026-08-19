/**
 * @file lvgl_mem.c
 * @brief LVGL 自定义内存分配 — 对象/绘制缓冲走 PSRAM
 *
 * 默认 CONFIG_LV_USE_BUILTIN_MALLOC 是 64KB 静态池占内部 SRAM
 * (实测整机 SRAM 空闲仅 45KB); 改为 LV_STDLIB_CUSTOM 后由本文件
 * 提供 lv_malloc_core/lv_free_core/lv_realloc_core, 全部走 PSRAM 堆,
 * 内部 SRAM 空闲提升至 ~109KB。
 * 显示 DMA 缓冲由 esp_lvgl_port 单独分配 (内部 DMA-capable 内存),
 * 不受此改动影响。
 */
#include "esp_heap_caps.h"
#include "stdlib/lv_mem.h"   /* lv_result_t 经 lv_types.h 一并引入 */

void * lv_malloc_core(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

void lv_free_core(void * p)
{
    heap_caps_free(p);
}

void * lv_realloc_core(void * p, size_t new_size)
{
    return heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM);
}

/* lv_mem_monitor/lv_mem_test 的 core 实现 (perf monitor 未启用, 提供以防链接引用) */
void lv_mem_monitor_core(lv_mem_monitor_t * mon_p)
{
    if (mon_p) {
        mon_p->total_size       = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        mon_p->free_size        = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        mon_p->free_biggest_size = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        mon_p->free_cnt = 0;
        mon_p->used_cnt = 0;
        mon_p->max_used = 0;
        mon_p->used_pct = 0;
        mon_p->frag_pct = 0;
    }
}

lv_result_t lv_mem_test_core(void)
{
    void *p = lv_malloc_core(64);
    if (!p) return LV_RESULT_INVALID;
    lv_free_core(p);
    return LV_RESULT_OK;
}
