/** @file power_manager.h @brief 电源管理: 轻睡眠 + 屏幕关闭降载 接口 */
#pragma once
#include "esp_err.h"
#include <stdbool.h>

/** 初始化电源管理:
 * - esp_pm_configure(true) 一次使能轻睡眠框架 (v2.5: 运行时开关改用 PM 锁,
 * configure(false) 在临界区内打日志会 abort — IDF 的坑)
 * - 注册轻睡眠进出日志回调 (CONFIG_PM_LIGHT_SLEEP_CALLBACKS) */
esp_err_t power_manager_init(void);

/** 屏幕彻底关闭 (息屏): 暂停动画帧定时器 + 禁用 LVGL 刷新 —
 * 画面静止后无脏区, tickless idle 才能获得更深的睡眠窗口。
 * 幂等; 必须在 LVGL 上下文 (持锁或能持锁) 调用, 禁止 ISR。 */
void power_manager_screen_off(void);

/** 屏幕唤醒: 恢复动画 + 恢复刷新 + 全屏重绘 (先 enable 再 invalidate,
 * 禁用期 invalidate 被丢弃 — 同 screen_switch.c 模式)。幂等。 */
void power_manager_screen_on(void);

/** 深度睡眠 (预留): 后续版本实现 (唤醒=重启, 需处理会话/记忆/重连恢复)。 */
esp_err_t power_manager_deep_sleep(uint32_t timeout_ms);

/** 息屏诊断: 轻睡进出计数 + esp_pm_dump_locks (长持锁 = 轻睡被禁)。调试期用。 */
void power_manager_dump_stats(void);

/** v2 诊断: vApplicationSleep 回调内窗口统计 (睡眠窗口 >=30000us 即会入睡)。
 * 回调在 idle 临界区, 只做整数统计, 由主循环安全上下文读取。 */
void power_manager_get_sleep_stats(uint32_t *total, uint32_t *over30, int64_t *max, int64_t *last);

/** : 未入睡计数 (exit_cb 收到 slept≈0 — 窗口不足或 esp_light_sleep_start 被拒)。
 * 离线诊断: 与 sleeps 配合区分 "锁未放" vs "尝试了但没睡成"。 */
uint32_t power_manager_get_rejects(void);

/** : vApplicationSleep 调用计数探针 — 自注册进 periph skip 回调列表,
 * 每次 should_skip 评估被调用 (pm_impl.c:809)。区分:
 * - 计数=0 → vApplicationSleep 从未被调用 (tick 列表被 <3ms 事件霸占)
 * - 计数>0 且 sleeps=0 → skip 恒 true (锁未放/periph skip) */
uint32_t power_manager_get_sleep_probe(void);

/** v2: 硬件触摸唤醒消费 — 轻睡退出回调 (pm_exit_cb) 检测到
 * ESP_SLEEP_WAKEUP_TOUCHPAD 时置位。不能直接在主循环轮询
 * esp_sleep_get_wakeup_cause: 该值粘滞 (亮屏期无睡眠不再覆盖, 保持上次
 * 触摸值), 息屏后立刻轮询会误报。回调只在真实睡眠退出时执行, 无假唤醒。
 * 主循环 1s 轮询调用, 读后即清。pad 输出唤醒通道 (诊断, 可能无效值)。 */
bool power_manager_touch_woke(uint32_t *pad);

/** : 最后一次睡眠退出的唤醒原因码 (esp_sleep_get_wakeup_cause)。
 * 修复后预期恒 ESP_SLEEP_WAKEUP_TIMER (4) — 定时器唤醒。CSV wk 列。 */
uint32_t power_manager_get_wake_cause(void);

/** : USB 连接感知禁睡 — USB-SERIAL-JTAG 的 SOF 帧存在 (主机在
 * 通信) 时持 NO_LIGHT_SLEEP 锁, 息屏轻睡不会冻结串口 (COM 掉口 →
 * 误判"卡死", 插着 USB 静置实测)。SOF 消失 (拔线) 释放恢复轻睡。
 * main.c 主循环 100ms 块检测 SOF 翻转调用, 幂等。 */
void power_manager_usb_connection(bool connected);
