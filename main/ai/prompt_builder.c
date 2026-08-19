/**
 * @file prompt_builder.c
 * @brief 构建服务端LLM请求上下文JSON: 宠物状态 + 传感器数据 + 近期对话摘要 + 用户设置
 *
 * ESP32仅上传原始数据, 服务端负责完整的Prompt模板拼装(Jinja2模板).
 */
#include "esp_err.h"
#include "esp_log.h"
static const char *TAG = "prompt_builder";
esp_err_t prompt_builder_init(void) { ESP_LOGI(TAG, "Prompt构建器 (桩)"); return ESP_OK; }
