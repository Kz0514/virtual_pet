/**
 * @file http_util.h
 * @brief HTTP GET 统一封装 — 同步块式获取 (esp_http_client)
 *
 * ④-7a 自 app/diary_sync 抽出共享; main.c 天气客户端同用。
 * 接口取舍: 只做"拿全文"一件事; 需流式/自定义头的场景各自另写。
 */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 同步 GET → PSRAM 动态缓冲 (上限 cap, 截断至 cap−1), 末尾补 '\0'。
 *  失败 (网络错误/非 HTTP 200/缓冲不足) 返回 NULL — 调用方 free() 归还。 */
char *http_get(const char *url, size_t cap);

#ifdef __cplusplus
}
#endif