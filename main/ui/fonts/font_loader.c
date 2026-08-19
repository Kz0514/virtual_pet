/** @file font_loader.c @brief SPIFFS→SPIRAM→lv_binfont_create_from_buffer */
#include "font_loader.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static const char *TAG = "font_load";
lv_font_t * g_zh_font = NULL;

typedef struct { uint8_t *buf; uint32_t size; } load_arg_t;

static void load_task(void *arg) {
    load_arg_t *a = (load_arg_t *)arg;
    g_zh_font = lv_binfont_create_from_buffer(a->buf, a->size);
    /* lv_binfont_create_from_buffer 内部将 cmap/glyph 数据全部复制到
     * LVGL 自有内存, 返回后不再引用源 buffer — 可安全释放 (省 2.7MB PSRAM) */
    free(a->buf);
    free(a);
    vTaskDelete(NULL);
}

void font_loader_init(void) {
    FILE *f = fopen("/spiffs/zh.bin", "rb");
    if (!f) {
        ESP_LOGW(TAG, "zh.bin 不存在, 中文字体未加载");
        return;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    load_arg_t *a = malloc(sizeof(load_arg_t));
    a->buf = heap_caps_malloc(fsize, MALLOC_CAP_SPIRAM);
    if (!a->buf) { ESP_LOGE(TAG, "SPIRAM 不足"); free(a); fclose(f); return; }
    a->size = (uint32_t)fsize;

    if (fread(a->buf, 1, fsize, f) != (size_t)fsize) {
        ESP_LOGE(TAG, "读取失败"); free(a->buf); free(a); fclose(f); return;
    }
    fclose(f);
    ESP_LOGI(TAG, "从 SPIFFS 加载 zh.bin (%ld bytes)", fsize);

    /* 暂停 TWDT, 在 CPU1 解析 (避免阻塞 CPU0 IDLE) */
    esp_task_wdt_deinit();
    TaskHandle_t task_h = NULL;
    xTaskCreatePinnedToCore(load_task, "fontld", 16384, a, 1, &task_h, 1);

    if (task_h) {
        TickType_t dl = xTaskGetTickCount() + pdMS_TO_TICKS(30000);
        while (eTaskGetState(task_h) != eDeleted && xTaskGetTickCount() < dl)
            vTaskDelay(pdMS_TO_TICKS(200));
    }

    /* 恢复 TWDT */
    esp_task_wdt_config_t twdt_cfg = {
        .timeout_ms = 5000,
        .idle_core_mask = (1 << 0) | (1 << 1),
        .trigger_panic = true,
    };
    esp_task_wdt_init(&twdt_cfg);

    if (g_zh_font) ESP_LOGI(TAG, "字体就绪");
    else           ESP_LOGW(TAG, "加载失败, 中文字体不可用");
}
