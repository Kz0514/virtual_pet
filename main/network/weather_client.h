/**
 * @file weather_client.h
 * @brief 天气客户端 — 开机单次拉取 (ip_location → adcode → current)
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** 开机天气拉取: 两次 GET, 结果仅日志 (📍城市 / 🌤天气), 失败静默。
 *  无节流 (开机只调一次) — 调用方保证注册成功后单次调用。 */
void weather_fetch_once(void);

#ifdef __cplusplus
}
#endif