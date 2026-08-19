/**
 * @file event_bus.c
 * @brief 内部事件总线 — FreeRTOS 任务间通信
 *
 * 消费者订阅感兴趣的事件类型, 生产者发布事件.
 * 事件类型: 触摸手势、摇动检测、唤醒词、传感器阈值、服务器消息、定时器、低电量、网络状态
 */
#include "esp_err.h"
#include "esp_log.h"
#include "event_bus.h"
static const char *TAG = "event_bus";
esp_err_t event_bus_init(void) { ESP_LOGI(TAG, "事件总线初始化 (桩)"); return ESP_OK; }
esp_err_t event_bus_publish(event_type_t type, void *data) { return ESP_OK; }
