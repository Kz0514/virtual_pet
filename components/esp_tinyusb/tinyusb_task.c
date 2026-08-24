/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "soc/soc_caps.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h" // Virtualpet 补丁: MALLOC_CAP_SPIRAM 任务栈
#include "tinyusb.h"
#include "sdkconfig.h"
#include "descriptors_control.h"

#if TUSB_VERSION_NUMBER < 1900          // < 0.19.0
#define tusb_deinit(x) tusb_teardown(x) // For compatibility with tinyusb component versions from 0.17.0~2 to 0.18.0~5
#endif

const static char *TAG = "tinyusb_task";

static portMUX_TYPE tusb_task_lock = portMUX_INITIALIZER_UNLOCKED;
#define TINYUSB_TASK_ENTER_CRITICAL() portENTER_CRITICAL(&tusb_task_lock)
#define TINYUSB_TASK_EXIT_CRITICAL() portEXIT_CRITICAL(&tusb_task_lock)

#define TINYUSB_TASK_CHECK(cond, ret_val) ({ \
    if (!(cond)) {                           \
        return (ret_val);                    \
    }                                        \
})

#define TINYUSB_TASK_CHECK_FROM_CRIT(cond, ret_val) ({ \
    if (!(cond)) {                                     \
        TINYUSB_TASK_EXIT_CRITICAL();                  \
        return ret_val;                                \
    }                                                  \
})

// TinyUSB task context
typedef struct {
    // TinyUSB stack configuration
    uint8_t rhport;                        /*!< USB Peripheral hardware port number. Available when hardware has several available peripherals. */
    tusb_rhport_init_t rhport_init;        /*!< USB Device RH port initialization configuration pointer */
    const tinyusb_desc_config_t *desc_cfg; /*!< USB Device descriptors configuration pointer */
    // Task related
    TaskHandle_t handle;                   /*!< Task handle */
    volatile TaskHandle_t awaiting_handle; /*!< Task handle, waiting to be notified after successful start of TinyUSB stack */
} tinyusb_task_ctx_t;

static bool _task_is_running = false;              // Locking flag for the task, access only from the critical section
static tinyusb_task_ctx_t *p_tusb_task_ctx = NULL; // TinyUSB task context

/* Virtualpet 补丁 (1.0.232): 静态 8K 栈替代动态分配。
 * 1.0.231 实测: 运行期内部堆稳态仅 ~4.6KB 空闲 / 最大连续块 ~4KB (1.0.226
 * 时 ~18.8KB), xTaskCreatePinnedToCore 动态分配 8K 栈 + TCB (需 ~8.3KB 连续
 * 内部) 必失败 → U盘模式 enter 死循环失败 (失败日志不可见: usb_new_phy 已把
 * PHY 切到 OTG, console 先死, restore 在错误日志之后才执行)。
 * 静态数组在 .bss = 内部 DRAM: 非 cache 后备, 与 flash 写冻结窗口无关
 * (铁律: 可调度任务栈必须内部 RAM — 1.0.223/1.0.225 双异常教训); 零运行期
 * 分配、碎片无关、永不失配。成本: 8K 静态 RAM 常驻 (boot 后内部堆 ~80KB,
 * 可承受)。若改 usb_storage.c 的 TINYUSB_TASK_STACK, 此处须同步 —
 * 大小不符在 tinyusb_task_start 直接报错 (见下)。 */
#define TINYUSB_TASK_STATIC_STACK_SIZE 8192
static StackType_t s_tusb_task_stack[TINYUSB_TASK_STATIC_STACK_SIZE / sizeof(StackType_t)];
static StaticTask_t s_tusb_task_tcb;

/**
 * @brief This top level thread processes all usb events and invokes callbacks
 */
static void tinyusb_device_task(void *arg)
{
    tinyusb_task_ctx_t *task_ctx = (tinyusb_task_ctx_t *)arg;

    // Sanity check
    assert(task_ctx != NULL);
    assert(task_ctx->awaiting_handle != NULL);

    ESP_LOGD(TAG, "TinyUSB task started");

    if (tud_inited()) {
        ESP_LOGE(TAG, "TinyUSB stack is already initialized");
        goto del;
    }
    if (tinyusb_descriptors_set(task_ctx->rhport, task_ctx->desc_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB descriptors set failed");
        goto del;
    }
    if (!tusb_rhport_init(task_ctx->rhport, &task_ctx->rhport_init)) {
        ESP_LOGE(TAG, "Init TinyUSB stack failed");
        goto desc_free;
    }

    TINYUSB_TASK_ENTER_CRITICAL();
    task_ctx->handle = xTaskGetCurrentTaskHandle(); // Save task handle
    p_tusb_task_ctx = task_ctx;                     // Save global task context pointer
    TINYUSB_TASK_EXIT_CRITICAL();

    xTaskNotifyGive(task_ctx->awaiting_handle); // Notify parent task that TinyUSB stack was started successfully

    while (1) { // RTOS forever loop
        tud_task();
    }

desc_free:
    tinyusb_descriptors_free();
del:
    TINYUSB_TASK_ENTER_CRITICAL();
    _task_is_running = false; // Task is not running anymore
    TINYUSB_TASK_EXIT_CRITICAL();
    vTaskDelete(NULL);
    // No return needed here: vTaskDelete(NULL) does not return
}

esp_err_t tinyusb_task_check_config(const tinyusb_task_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "Task configuration can't be NULL");
    ESP_RETURN_ON_FALSE(config->size != 0, ESP_ERR_INVALID_ARG, TAG, "Task size can't be 0");
    ESP_RETURN_ON_FALSE(config->priority != 0, ESP_ERR_INVALID_ARG, TAG, "Task priority can't be 0");
#if CONFIG_FREERTOS_UNICORE
    ESP_RETURN_ON_FALSE(config->xCoreID == 0, ESP_ERR_INVALID_ARG, TAG, "Task affinity must be 0 only in uniprocessor mode");
#else
    ESP_RETURN_ON_FALSE(config->xCoreID <= SOC_CPU_CORES_NUM, ESP_ERR_INVALID_ARG, TAG, "Task affinity should be less or equal to CPU amount");
#endif //
    return ESP_OK;
}

esp_err_t tinyusb_task_start(tinyusb_port_t port, const tinyusb_task_config_t *config, const tinyusb_desc_config_t *desc_cfg)
{
    ESP_RETURN_ON_ERROR(tinyusb_descriptors_check(port, desc_cfg), TAG, "TinyUSB descriptors check failed");

    TINYUSB_TASK_ENTER_CRITICAL();
    TINYUSB_TASK_CHECK_FROM_CRIT(p_tusb_task_ctx == NULL, ESP_ERR_INVALID_STATE); // Task shouldn't started
    TINYUSB_TASK_CHECK_FROM_CRIT(!_task_is_running, ESP_ERR_INVALID_STATE);       // Task shouldn't be running
    _task_is_running = true;                                                      // Task is running flag, will be cleared in task in case of the error
    TINYUSB_TASK_EXIT_CRITICAL();

    esp_err_t ret;
    tinyusb_task_ctx_t *task_ctx = heap_caps_calloc(1, sizeof(tinyusb_task_ctx_t), MALLOC_CAP_DEFAULT);
    if (task_ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }

    task_ctx->awaiting_handle = xTaskGetCurrentTaskHandle(); // Save parent task handle
    task_ctx->handle = NULL;                                 // TinyUSB task is not started
    task_ctx->rhport = port;                                 // Peripheral port number
    task_ctx->rhport_init.role = TUSB_ROLE_DEVICE;           // Role selection: esp_tinyusb is always a device
    // Speed selection: ESP32-S31 is HS-only single-port chip
#if CONFIG_IDF_TARGET_ESP32S31
    task_ctx->rhport_init.speed = TUSB_SPEED_HIGH;
#else
    task_ctx->rhport_init.speed = (port == TINYUSB_PORT_FULL_SPEED_0) ? TUSB_SPEED_FULL : TUSB_SPEED_HIGH;
#endif
    task_ctx->desc_cfg = desc_cfg;

    TaskHandle_t task_hdl = NULL;
    ESP_LOGD(TAG, "Creating TinyUSB main task on CPU%d", config->xCoreID);
    /* Virtualpet 本地补丁 (1.0.232): 静态栈 + xTaskCreateStaticPinnedToCore —
     * 见 s_tusb_task_stack 定义处注释 (动态 8K 栈在碎片化内部堆上必失败)。
     * 历史: 1.0.222 PSRAM 栈 (16384 阈值时代) → 1.0.226 回内部动态 8K →
     * 1.0.232 静态内部 8K (运行期堆碎片化, 最大块 ~4KB, 动态分配失败)。
     * 栈深度按 words = 静态字节数 / sizeof(StackType_t)。 */
    if (config->size != TINYUSB_TASK_STATIC_STACK_SIZE) {
        ESP_LOGE(TAG, "TinyUSB 静态栈大小不符: config->size=%u 静态=%u — 同步 usb_storage.c",
                 (unsigned)config->size, (unsigned)TINYUSB_TASK_STATIC_STACK_SIZE);
        ret = ESP_ERR_INVALID_ARG;
        goto err;
    }
    task_hdl = xTaskCreateStaticPinnedToCore(tinyusb_device_task,
                                             "TinyUSB",
                                             TINYUSB_TASK_STATIC_STACK_SIZE / sizeof(StackType_t),
                                             (void *)task_ctx,
                                             config->priority,
                                             s_tusb_task_stack,
                                             &s_tusb_task_tcb,
                                             config->xCoreID);
    if (task_hdl == NULL) {
        ESP_LOGE(TAG, "Create TinyUSB main task failed");
        ret = ESP_ERR_NOT_FINISHED;
        goto err;
    }

    // Wait until the Task notify that port is active, 5 sec is more than enough
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000)) == 0) {
        ESP_LOGE(TAG, "Task wasn't able to start TinyUSB stack");
        ret = ESP_ERR_TIMEOUT;
        goto err;
    }

    return ESP_OK;

err:
    heap_caps_free(task_ctx);
    return ret;
}

esp_err_t tinyusb_task_stop(void)
{
    TINYUSB_TASK_ENTER_CRITICAL();
    TINYUSB_TASK_CHECK_FROM_CRIT(p_tusb_task_ctx != NULL, ESP_ERR_INVALID_STATE);
    tinyusb_task_ctx_t *task_ctx = p_tusb_task_ctx;
    p_tusb_task_ctx = NULL;
    _task_is_running = false;
    TINYUSB_TASK_EXIT_CRITICAL();

    if (task_ctx->handle != NULL) {
        vTaskDelete(task_ctx->handle);
        task_ctx->handle = NULL;
    }
    // Free descriptors
    tinyusb_descriptors_free();
    // Stop TinyUSB stack
    ESP_RETURN_ON_FALSE(tusb_deinit(task_ctx->rhport), ESP_ERR_NOT_FINISHED, TAG, "Unable to teardown TinyUSB stack");
    // Cleanup
    heap_caps_free(task_ctx);
    return ESP_OK;
}
