/**
 * @file wifi_manager.h
 * @brief WiFi 连接管理 + SoftAP 配网门户
 *
 * 自动流程: NVS有凭据→直连 / 无凭据→开启SoftAP配网
 * Captive Portal: DNS劫持所有域名→HTTP配网页→保存密码→重连
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_DISCONNECTED,
    WIFI_CONNECTING,
    WIFI_CONNECTED,
    WIFI_SOFTAP_MODE, /* 配网模式 */
} wifi_state_t;

esp_err_t wifi_manager_init(void);
wifi_state_t wifi_get_state(void);
bool wifi_is_connected(void);
const char *wifi_get_ip(void);

/** 息屏挂起 (esp_wifi_stop 前调用): 停重连超时定时器 + 状态归位 — 防止
 * stop 触发的 DISCONNECTED 事件带着 CONNECTING 状态误跑重连/切网流程
 * (WiFi 已停, set_mode 失败刷错误日志)。 */
void wifi_manager_suspend(void);

/** 亮屏恢复 (esp_wifi_start 后调用): 重新走完整连接流程 (set_mode →
 * set_config → connect)。SoftAP 配网态 / 已连接态不动。 */
void wifi_manager_resume(void);

/** : 息屏停 WiFi 后注销轻睡 skip 回调 — esp_wifi_init (wifi_init.c:407)
 * 注册 esp_wifi_internal_is_tsf_active, 但只在 esp_wifi_deinit (:235) 注销,
 * 息屏路径 (esp_wifi_stop) 永不 deinit → 回调常驻。若 stop 后 TSF 标志仍
 * 置位 (连接中直接 stop), 轻睡判定 (pm_impl.c:1038/:829) 恒 skip → rtos 锁
 * 永不放 + 轻睡 100% 不进入。WiFi 已停, 回调对判定无意义, 注销零副作用。
 * 亮屏后不恢复: 亮屏期持有 screen 禁睡锁, 该回调仅息屏期才有意义。 */
void wifi_manager_detach_light_sleep_skip(void);

/** ws 连接尝试失败通知 (仅"连接从未成功"的失败 — 已连后断开由组件自动
 * 重连, 不通知)。连续 WS_FAIL_THRESHOLD 次 → 控制任务 DNS 自检: 通过 =
 * 服务器异常 (不动, ws 继续自动重试); 失败 = 数据面死 (僵尸链路) →
 * 强制断开重连。无定时器无周期探活 — 仅在真实连接异常时自愈。 */
void wifi_manager_note_ws_fail(void);

#ifdef __cplusplus
}
#endif
