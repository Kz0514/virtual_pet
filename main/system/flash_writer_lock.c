/**
 * @file flash_writer_lock.c
 * @brief 全局 flash 写互斥 + 读优先让位 — 链接器 --wrap 拦截 esp_flash API
 *
 * 背景: 各文件系统 (FATFS wl / LittleFS / NVS / SPIFFS) 各自加锁互不相干,
 * 擦除 (esp_flash_erase_region) 内部 yield 窗口释放 esp_flash 锁期间,
 * 另一任务调 esp_flash API 撞硬件 busy (ESP_ERR_INVALID_STATE 0x101)
 * → 上层重试风暴 → TASK_WDT。本模块用 --wrap 把三个 esp_flash API
 * 全局拦截 (所有 FS 底层最终都调它们), 统一递归互斥:
 * 擦除持锁跨 yield → 窗口内其他访问在锁上排队等待, 0x101 不可能出现。
 *
 * 读优先: 擦除拆 4KB 片 (esp_flash_erase_region 最小粒度), 片间检查
 * 读等待者 → 放锁 1 tick 让读插队 (动画素材/SPIFFS 读取优先于写),
 * 每片最多让位 1 次 + 每次擦除调用预算上限, 防高优先级读者持续
 * 插队饿死擦除。
 *
 * 守卫: 锁未初始化 / 调度器未启动 / ISR 上下文 → 直接透传 (这些
 * 阶段无并发, 免锁安全)。
 */
#include "flash_writer_lock.h"
#include "esp_attr.h"
#include "esp_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* __real_ 符号由链接器 --wrap 生成, 指向原始函数 */
esp_err_t __real_esp_flash_read(esp_flash_t *chip, void *dst,
                                uint32_t src_offset, size_t size);
esp_err_t __real_esp_flash_write(esp_flash_t *chip, const void *src,
                                 uint32_t dst_offset, size_t size);
esp_err_t __real_esp_flash_erase_region(esp_flash_t *chip,
                                        uint32_t start_addr, uint32_t len);

static SemaphoreHandle_t s_mutex; /* 递归互斥 — 防 IDF 内部嵌套调用 */

/* 正在等锁的读请求数 (让位判定) — DRAM: 擦除片间检查点 cache 已恢复,
 * 但保守起见放 DRAM 保证任何时刻可访问 */
static DRAM_ATTR volatile int s_readers_waiting;
static portMUX_TYPE s_count_mux = portMUX_INITIALIZER_UNLOCKED;

/* 让位预算 — 每次擦除调用上限, 防高优先级读者持续插队饿死擦除 */
#define YIELD_BUDGET 16
static int s_yield_budget;

void flash_writer_lock_init(void)
{
    if (!s_mutex) s_mutex = xSemaphoreCreateRecursiveMutex();
}

static inline bool flash_lock_ready(void)
{
    return s_mutex != NULL &&
           xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
           !xPortInIsrContext();
}

static inline void readers_inc(void)
{
    portENTER_CRITICAL(&s_count_mux);
    s_readers_waiting++;
    portEXIT_CRITICAL(&s_count_mux);
}

static inline void readers_dec(void)
{
    portENTER_CRITICAL(&s_count_mux);
    s_readers_waiting--;
    portEXIT_CRITICAL(&s_count_mux);
}

static inline int readers_pending(void)
{
    int v;
    portENTER_CRITICAL(&s_count_mux);
    v = s_readers_waiting;
    portEXIT_CRITICAL(&s_count_mux);
    return v;
}

static inline void flash_lock(void) { xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY); }
static inline void flash_unlock(void) { xSemaphoreGiveRecursive(s_mutex); }

esp_err_t __wrap_esp_flash_read(esp_flash_t *chip, void *dst,
                                uint32_t src, size_t size)
{
    if (!flash_lock_ready()) return __real_esp_flash_read(chip, dst, src, size);
    readers_inc(); /* 标记读等待 — 擦除片间检查此计数让位 */
    flash_lock();
    esp_err_t ret = __real_esp_flash_read(chip, dst, src, size);
    flash_unlock();
    readers_dec();
    return ret;
}

esp_err_t __wrap_esp_flash_write(esp_flash_t *chip, const void *src,
                                 uint32_t dst, size_t size)
{
    if (!flash_lock_ready()) return __real_esp_flash_write(chip, src, dst, size);
    flash_lock();
    esp_err_t ret = __real_esp_flash_write(chip, src, dst, size);
    flash_unlock();
    return ret;
}

esp_err_t __wrap_esp_flash_erase_region(esp_flash_t *chip,
                                        uint32_t start, uint32_t len)
{
    if (!flash_lock_ready()) return __real_esp_flash_erase_region(chip, start, len);
    flash_lock();
    esp_err_t ret = ESP_OK;

    /* 非 4KB 对齐 → 原样透传 (保持 IDF 对齐语义, 不做拆片让位) */
    if ((start & 0xFFF) != 0 || (len & 0xFFF) != 0) {
        ret = __real_esp_flash_erase_region(chip, start, len);
    } else {
        /* 拆 4KB 片: 片间 (cache 已恢复) 检查读等待者 → 放锁让读插队 */
        s_yield_budget = YIELD_BUDGET;
        for (uint32_t off = 0; off < len; off += 4096) {
            ret = __real_esp_flash_erase_region(chip, start + off, 4096);
            if (ret != ESP_OK) break;
            if (readers_pending() > 0 && s_yield_budget > 0) {
                s_yield_budget--;
                flash_unlock();
                vTaskDelay(1); /* 让调度器切到读任务 (读优先于写) */
                flash_lock();
            }
        }
    }
    flash_unlock();
    return ret;
}
