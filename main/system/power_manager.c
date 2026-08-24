/**
 * @file power_manager.c
 * @brief 电源管理: 轻睡眠 (esp_pm + tickless idle) + 屏幕关闭降载
 *
 * 设计 (2026-08-22, 实测迭代):
 * - 轻睡眠走自动路径: CONFIG_PM_ENABLE + tickless idle, 空闲即睡,
 * 任何中断/定时器自然唤醒 — 不需要手动管理唤醒源
 * - 运行时开关 = PM 锁: esp_pm_configure(true) 只在 init 调一次,
 * 之后 acquire(亮屏禁睡)/release(息屏允许睡) — configure(false) 在临界区
 * 内打日志会 abort (pm_impl.c:527 → sleep_modes.c:1725, 实测)
 * - 实测: 亮屏期轻睡与背光/渲染交互导致屏幕闪烁 → 屏幕可见时禁睡
 * (screen_on 态), 完全息屏后开睡 (电流大头在息屏段)
 * - 正式版 (2026-08-22): 息屏停 LVGL tick (lvgl_port_stop, 解除 5ms
 * esp_timer 窗口限制) + 停 50Hz 触摸扫描 (FreeRTOS 定时器 20ms 限制)
 * → 睡眠窗口 ~1s (H0/H1 心跳); 触摸唤醒走硬件路径: 息屏时
 * esp_sleep_enable_touchpad_wakeup + 按软件基线重调唤醒阈值,
 * pm_exit_cb 检测 ESP_SLEEP_WAKEUP_TOUCHPAD 置位, 主循环消费
 * - (2026-08-22): 弃用硬件触摸唤醒 (比较器基准冻结, 入睡瞬间
 * 即触发) → 全走软件路径 (10Hz 兜底扫描 + 摇动), 见 touch_fpc_pause
 * - 息屏关 PA (2026-08-23): 静音不停 PA 的残余漂移经喇叭电磁耦合
 * 右侧触摸通道, delta 275 超阈值假唤醒 (CSV 实锤) — 见 screen_off
 * - TTS 播放/录音期由 tts_client/voice_chat/session_mgr 持 PM 锁
 * 禁止轻睡 (冻结 I2S DMA 会破音/丢录音), 见各模块
 * - 深度睡眠预留: 唤醒=重启, 需处理会话/记忆/重连, 后续版本实现
 */
#include "power_manager.h"
#include "st7789.h" /* : 息屏面板进 SLPIN (内部 ~119Hz 扫描耗电) */
#include "es8311_drv.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_private/pm_impl.h" /* : skip 回调注册 (探针) */
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "esp_system.h" /* esp_reset_reason — boot 复位原因诊断 */
#include "nvs_flash.h"  /* boot 计数读取 (重启对账) */
#include <fcntl.h>      /* open 标志 */
#include <unistd.h>     /* write/close */
#include <stdio.h>      /* snprintf */
#include "lvgl.h"
#include "pet_avatar.h"
#include "touch_fpc.h"
#include "dmp_mpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "power_manager";

static bool s_off = false; /* 屏幕是否处于彻底关闭态 (幂等去重) */

/* 轻睡进出计数 — 息屏 60s 周期诊断打印 (确认轻睡在发生) */
static volatile uint32_t s_sleeps = 0;
static volatile uint32_t s_wakes = 0;

/* 轻睡眠进出回调 — ★ 运行在 idle 任务 vApplicationSleep 的
 * portENTER_CRITICAL(&s_switch_lock) 临界区内 (pm_impl.c:858), 中断已屏蔽:
 * - xPortCanYield==false → newlib 锁视作 ISR 上下文
 * - 任何 ESP_LOGI → _lock_acquire_recursive → lock_acquire_generic
 * (locks.c:145) 直接 abort — 息屏死机根因, 2026-08-22 backtrace 确证
 * - sleep_time_us = vApplicationSleep 计算后的睡眠窗口 (MIN(wakeup_delay,
 * esp_timer 最早非SKIP alarm)) — ≥30000 即会真正尝试入睡
 * ★ 回调内禁止一切日志/输出/阻塞, 只允许纯计数器/整数统计递增。
 * 统计供主循环每 2s 打印 (安全上下文) — 定性死因: 窗口是否曾 ≥30ms。 */
static volatile uint32_t s_win_total = 0;  /* 回调执行次数 */
static volatile uint32_t s_win_over30 = 0; /* sleep_time_us >= 30000 次数 */
static volatile int64_t s_win_max = 0;     /* 历史最大窗口 */
static volatile int64_t s_win_last = 0;    /* 最后一次窗口 */

static esp_err_t pm_enter_cb(int64_t sleep_time_us, void *arg)
{
    s_sleeps++;
    s_win_total++;
    if (sleep_time_us >= 30000LL) s_win_over30++;
    if (sleep_time_us > s_win_max) s_win_max = sleep_time_us;
    s_win_last = sleep_time_us;
    return ESP_OK;
}

void power_manager_get_sleep_stats(uint32_t *total, uint32_t *over30, int64_t *max, int64_t *last)
{
    *total = s_win_total;
    *over30 = s_win_over30;
    *max = s_win_max;
    *last = s_win_last;
}

/* : 未入睡计数 — exit_cb 收到 slept_us≈0 → vApplicationSleep 评估了
 * 但未真正入睡 (睡眠窗口 <3ms 阈值 或 esp_light_sleep_start 被拒)。
 * 区分 "锁未放 (enter_cb 不执行)" vs "尝试了但没睡成"。离线诊断列。 */
static volatile uint32_t s_rejects = 0;

uint32_t power_manager_get_rejects(void)
{
    return s_rejects;
}

/* 探针: vApplicationSleep 调用计数 — 注册进 periph skip 回调列表,
 * 每次 should_skip_light_sleep 评估 (pm_impl.c:829) 时被调用。纯计数器,
 * IRAM 临界区安全 (回调在 s_switch_lock 临界区内执行, 只做整数自增)。
 * 意义: sleeps=0 且此计数=0 → vApplicationSleep 从未被调用 (tick 列表被
 * <3ms 事件霸占, tasks.c:4386 阈值未达); 此计数>0 且 sleeps=0 → skip 恒真。 */
static volatile uint32_t s_sleep_probe = 0;

static bool IRAM_ATTR pm_sleep_probe_cb(void)
{
    s_sleep_probe++;
    return false; /* 探针不参与 skip 决策 */
}

uint32_t power_manager_get_sleep_probe(void)
{
    return s_sleep_probe;
}

/* 触摸唤醒标志 — 睡眠退出回调置位, 主循环消费 (读后即清)。 */
static volatile bool s_touch_woke = false;
static volatile uint32_t s_touch_wake_pad = 0;

/* : 最后唤醒原因码 (esp_sleep_get_wakeup_cause) — CSV wk 列。
 * 修复后预期: 恒 3 (ESP_SLEEP_WAKEUP_TIMER, 定时器唤醒), 4=触摸 (粘滞旧值),
 * 0=异常/未定义 */
static volatile uint32_t s_win_wake = 0;

uint32_t power_manager_get_wake_cause(void)
{
    return s_win_wake;
}

static esp_err_t pm_exit_cb(int64_t sleep_time_us, void *arg)
{
    s_wakes++;
    s_win_wake = (uint32_t)esp_sleep_get_wakeup_cause();
    /* : slept≈0 → 窗口不足或被拒 (esp_light_sleep_start 失败), 未真睡 */
    if (sleep_time_us < 1000LL) s_rejects++;
    /* 硬件触摸唤醒检测 — 只能在这里读唤醒源。esp_sleep_get_wakeup_cause
     * 粘滞 (亮屏期锁禁睡后不再覆盖, 一直保持上次触摸值), 主循环直接轮询
     * 会在息屏后立刻误报一次; 此回调只在真实睡眠退出时执行, 无假唤醒。
     * 回调在 idle 临界区: 只置标志, 由主循环 1s 轮询消费。 */
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TOUCHPAD) {
        s_touch_woke = true;
        s_touch_wake_pad = (uint32_t)esp_sleep_get_touchpad_wakeup_status();
    }
    return ESP_OK;
}

bool power_manager_touch_woke(uint32_t *pad)
{
    bool w = s_touch_woke;
    s_touch_woke = false;
    if (pad) *pad = s_touch_wake_pad;
    return w;
}

/* 息屏诊断: 轻睡计数 + 当前 PM 锁列表 (有长持锁 = 轻睡被禁) */
void power_manager_dump_stats(void)
{
    ESP_LOGI(TAG, "轻睡 %u 次 / 唤醒 %u 次", (unsigned)s_sleeps, (unsigned)s_wakes);
    esp_pm_dump_locks(stdout);
}

/* 亮屏禁睡锁 — acquire=禁睡 (防闪烁), release=允许入睡 (息屏)。
 * esp_pm_configure 反复切换的替代: 纯软件, 无时钟/唤醒源副作用。 */
static esp_pm_lock_handle_t s_screen_lock;

/* : USB 连接感知禁睡锁 — 息屏期轻睡会冻结 USB-SERIAL-JTAG 时钟,
 * 主机侧 COM 口消失 (插着 USB 静置息屏实测掉串口 → esptool 连不上,
 * 误判"卡死")。SOF 帧存在 = USB 主机在通信 → 持锁禁睡保活; 拔线后
 * SOF 消失 → 释放恢复轻睡 (纯插电态无省电意义, 拔线才是省电场景)。
 * main.c 主循环 100ms 块检测 SOF 翻转调用。与 U盘模式锁 (usb_storage
 * 持有, 恒持) 互补: U盘模式 = OTG, USJ 无 SOF, 本锁不参与。 */
static esp_pm_lock_handle_t s_usbconn_lock;

void power_manager_usb_connection(bool connected)
{
    if (!s_usbconn_lock) {
        if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usbconn",
                               &s_usbconn_lock) != ESP_OK) {
            ESP_LOGE(TAG, "USB 连接锁创建失败 — 息屏期 USB 串口会被轻睡冻结");
            return;
        }
    }
    esp_err_t ret = connected ? esp_pm_lock_acquire(s_usbconn_lock)
                              : esp_pm_lock_release(s_usbconn_lock);
    if (ret != ESP_OK)
        ESP_LOGE(TAG, "USB 连接锁 %s 失败: %s", connected ? "获取" : "释放",
                 esp_err_to_name(ret));
    else
        ESP_LOGI(TAG, "USB 连接锁: %s (SOF %s)", connected ? "获取" : "释放",
                 connected ? "存在" : "消失");
}

/* 运行时开关轻睡 — (2026-08-22): 改用 PM 锁替代 esp_pm_configure 切换。
 * 放弃 configure 的原因 (实测 abort, backtrace 确证):
 * esp_pm_configure(false) 内部 (pm_impl.c:527) 在 s_switch_lock 临界区内调用
 * esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER), 而该函数在 S3 上
 * TIMER 分支不匹配 → 兜底 else 分支 ESP_LOGE (sleep_modes.c:1725) → 临界区内
 * newlib 锁 abort (locks.c:145) — 唤醒即崩。"自动亮屏时断口"就是它。
 * IDF 的坑, 绕开: esp_pm_configure(true) 只在 init 调一次, 之后
 * acquire(亮屏禁睡)/release(息屏允许睡) 控制 — 锁切换纯软件 (s_mode_mask
 * + need_switch 重评估, 240==240 无频率切换), 不打日志不进临界区。 */
static void pm_set_light_sleep(bool enable)
{
    esp_err_t ret = enable ? esp_pm_lock_release(s_screen_lock)
                           : esp_pm_lock_acquire(s_screen_lock);
    if (ret != ESP_OK)
        ESP_LOGE(TAG, "轻睡=%d 锁操作失败: %s", enable, esp_err_to_name(ret));
    else
        ESP_LOGI(TAG, "轻睡: %s", enable ? "开" : "关");
}

/* 重启原因落盘 — 设备移动且重启偶发, 无法实时监视串口; 开机时把
 * esp_reset_reason 覆盖写入 /data/reset_reason.txt, 拷数据时一并带走。
 * 映射: 1=上电 2=软复位 6/7=崩溃 12=RTC看门狗 13=brownout。
 * VFS fd 路径 (禁 fopen); 仅 init 普通上下文调用 (挂载后)。 */
static void save_reset_reason(void)
{
    uint32_t boot_cnt = 0;
    nvs_handle_t bh;
    if (nvs_open("diag", NVS_READONLY, &bh) == ESP_OK) {
        nvs_get_u32(bh, "boot_cnt", &boot_cnt);
        nvs_close(bh);
    }
    int fd = open("/data/reset_reason.txt", O_CREAT | O_TRUNC | O_WRONLY);
    if (fd < 0)
        return; /* 挂载未就绪 — 下次 boot 再写 */
    char line[64];
    int n = snprintf(line, sizeof(line), "reset=%d boot=%u\n",
                     (int)esp_reset_reason(), (unsigned)boot_cnt);
    if (n > 0)
        write(fd, line, (size_t)n);
    close(fd);
}

esp_err_t power_manager_init(void)
{
    /* 轻睡框架一次就绪 (: 之后不再反复 configure — 见 pm_set_light_sleep
     * 注释)。init 普通上下文, configure 内临界区日志安全。 */
    esp_pm_config_t pm_cfg = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 240, /* 上下限相等 = 无 DFS, 纯轻睡 */
        .light_sleep_enable = true,
    };
    esp_err_t ret = esp_pm_configure(&pm_cfg);
    if (ret != ESP_OK)
        ESP_LOGE(TAG, "esp_pm_configure(轻睡) 失败: %s", esp_err_to_name(ret));

    esp_err_t lret = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "screen", &s_screen_lock);
    if (lret != ESP_OK)
        ESP_LOGE(TAG, "屏幕 PM 锁创建失败: %s", esp_err_to_name(lret));
    pm_set_light_sleep(false); /* 默认亮屏态: 禁睡, 防闪烁 */
#ifdef CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    /* 签名: 配置结构体而非三个参数 (旧签名已删除) */
    esp_pm_sleep_cbs_register_config_t cbs = {
        .enter_cb = pm_enter_cb,
        .exit_cb = pm_exit_cb,
    };
    esp_pm_light_sleep_register_cbs(&cbs);
#endif
    /* : vApplicationSleep 调用计数探针 — 槽位 2 个, WiFi 占 1, 余 1 */
    esp_err_t pret = esp_pm_register_skip_light_sleep_callback(pm_sleep_probe_cb);
    if (pret != ESP_OK)
        ESP_LOGE(TAG, "轻睡探针注册失败: %s", esp_err_to_name(pret));
    /* : boot 复位原因 — 实锤"插 USB 黑屏重启"根因 (brownout vs 崩溃)。
     * 映射: 1=电源上电 2=软件复位 3=看门狗 4=深度睡眠唤醒 5=安全
     * 6=core0 崩溃 7=core1 崩溃 8=light sleep 唤醒 9=CPU 错误
     * 10=外部复位 11=UART 下载 12=RTC 看门狗 13=brownout 检测 */
    ESP_LOGI(TAG, "复位原因: %d (1=上电 2=软复位 6/7=崩溃 12=RTC看门狗 13=brownout)",
             (int)esp_reset_reason());
    save_reset_reason(); /* 落盘 /data/reset_reason.txt — 拷数据时带走 */
    ESP_LOGI(TAG, "轻睡眠策略就绪 (亮屏禁睡, 息屏开睡, 240MHz 恒频)");
    return ESP_OK;
}

void power_manager_screen_off(void)
{
    if (s_off) return;
    s_off = true;

    lvgl_port_lock(0); /* 递归锁: 主循环 (无锁) 与 LVGL 定时器 (已持锁) 均安全 */
    lv_display_t *disp = lv_display_get_default();
    if (disp) lv_display_enable_invalidation(disp, false);
    pet_avatar_pause(); /* 停帧定时器 — 动画冻结在 PSRAM 帧缓冲 (不掉电) */
    /* : lvgl_port_stop 移进锁内 — 它现在 vTaskSuspend 渲染任务。
     * 锁内挂起保证 taskLVGL 未持 lvgl_mux (递归锁被挂起任务占用 =
     * 永锁死)。挂起后 taskLVGL 不再每 tick 就绪, uxTopReadyPriority
     * 归 0 → 轻睡路径解锁 (tasks.c prvGetExpectedIdleTime)。 */
    lvgl_port_stop(); /* 停 LVGL tick + lv_timers + 挂起渲染任务 */
    lvgl_port_unlock();

    /* 诊断版 (2026-08-22): 恢复息屏开轻睡 — 复现上一版"息屏后静默"死机,
     * 配合主循环每 2s 的 "PM窗" 探针抓死前窗口值, 定性死因:
     * - 窗口 ≥30000us → 睡眠尝试发生 → 死因在睡眠路径 (sleep_cpu_configure
     * 失败后仍入睡 + 唤醒失败 → RTC WDT 重启循环)
     * - 窗口恒 <30000 → 睡眠不可能发生 → 死因=esp_pm_configure(true) 副作用
     * 定性后回退禁睡或修根因, 再出正式版。 */
    /* 正式版: 真轻睡 — 解除 5ms/20ms 窗口限制。
     * (2026-08-22): 不再武装触摸硬件唤醒 — 拔电实测 97% 睡眠
     * slept<1ms 被弹回: 硬件比较器 (raw−硬件基准>阈值) 的基准在"触摸态"
     * 冻结不更新, 环境读数一旦超阈值 (CH9 硬件基准 37348 vs 软件基线
     * 43904) → 入睡瞬间比较器即触发 → 睡眠从未真正持续 → 电流 60mA 不降。
     * 基准无法写入 (ESP-IDF 无 set_benchmark API), 收敛手段 (临时放大
     * 阈值) 有"期间叫不醒"风险 → 弃用硬件唤醒, 触摸唤醒全走软件路径:
     * 1. 10Hz 兜底扫描 (main.c 主循环 100ms 周期, touch_fpc_poll_once
     * + filtered>250 判触摸 → 亮屏), 响应 ≤100ms
     * 2. 摇动唤醒: dmp_bg 每 50ms 读 FIFO, 摇动检测亮屏
     * 睡眠窗口 = min(main 100ms, dmp_bg 50ms) ≈ 50ms → CPU 断电 ~96%,
     * 电流大头剩外设 (WiFi/DMP/DAC)。
     * 顺序: 释放锁允许入睡 → 停 LVGL tick (esp_timer 5ms 窗口限制) →
     * 停 50Hz 扫描 (FreeRTOS 定时器 20ms 限制; 硬件 FSM 继续采样,
     * 10Hz 兜底扫描直读最新 raw)。 */
    pm_set_light_sleep(true); /* 画面静止: 开轻睡拿息屏电流大头 */
    touch_fpc_pause();        /* 停 50Hz 扫描 (窗口 20ms→50ms) */
    dmp_mpu_set_off(true);    /* DMP 轮询 50→250ms — 息屏消费者 (shake/tap) 已 gate,
                               * 稀释 FreeRTOS 到期点 (winname 58% 是 dmp_bg) */
    es8311_drv_mute(true);    /* 数字静音 — : DAC 常驻上电但 I2S 时钟已停,
                               * 无时钟输出漂移被常开 PA 放大 → 息屏噪音 (实测);
                               * mute bit 切换无模拟瞬态, 不违反 DAC 保持上电 */
    /* : 息屏彻底关 PA — 静音只停 DAC 输出, PA 仍放大残余漂移 → 喇叭
     * 电磁耦合右侧触摸通道 (CSV 实锤: 息屏段 d8-d11 delta 梯度 37→275,
     * 右侧阈值 200 被超 → 探针假唤醒自动亮屏)。TPA2011 数字关断无爆音
     * (输入已静音), 亮屏侧先解除静音再上电, 规避上电 POP */
    es8311_drv_pa_set(false);
    /* : 面板芯片进睡眠 — 背光已关但 ST7789 仍以 ~119Hz 内部全帧
     * 扫描 (normal mode ~3-5mA), 白烧电。DISPOFF+SLPIN 停振荡器。
     * SPI 传输在任务上下文完成 (轻睡已开但任务在跑, 不会入睡) */
    st7789_panel_sleep(true);
    ESP_LOGI(TAG, "屏幕已关闭: 动画暂停 + tick 停止 + 轻睡开启 + DAC 静音 + PA 关闭 + 面板睡眠");
}

void power_manager_screen_on(void)
{
    if (!s_off) return;
    s_off = false;

    pm_set_light_sleep(false); /* 先禁睡再恢复刷新 — 避免渲染期入睡 */
    /* : 先唤醒面板 (SLPOUT→120ms→DISPON) 再恢复刷新 — 否则 LVGL
     * 重绘内容落在 DISPOFF 状态白费; 面板 RAM 保持息屏前帧, 期间显示
     * 旧画面, 全屏重绘覆盖, 无闪烁 */
    st7789_panel_sleep(false);
    lvgl_port_resume();     /* 恢复 LVGL tick + lv_timers (窗口回 5ms, 动画恢复) */
    touch_fpc_resume();     /* 恢复 50Hz 扫描 (读数缓存 → 手势/滑动正常) */
    dmp_mpu_set_off(false); /* DMP 轮询回 50ms — 敲击/摇晃检测恢复全速 */
    es8311_drv_mute(false); /* 先解除 DAC 静音, 输出稳定后再上电 PA — 防上电 POP */
    es8311_drv_pa_set(true); /* : 恢复 PA — 与息屏关断配对 (见 screen_off) */
    s_touch_woke = false;   /* 残留唤醒标志丢弃 — 本次唤醒已由主循环处理 */

    lvgl_port_lock(0);
    pet_avatar_resume();
    lv_display_t *disp = lv_display_get_default();
    if (disp) {
        /* 顺序: 先恢复 invalidate 再全屏重绘 — 禁用期 invalidate 被丢弃 */
        lv_display_enable_invalidation(disp, true);
        lv_obj_invalidate(lv_screen_active());
    }
    lvgl_port_unlock();

    ESP_LOGI(TAG, "屏幕已唤醒: 动画恢复 + 全屏重绘");
}

esp_err_t power_manager_deep_sleep(uint32_t timeout_ms)
{
    ESP_LOGI(TAG, "深度睡眠预留 (唤醒=重启, 需处理会话/记忆/重连 — 后续版本实现, 超时 %lu ms)",
             (unsigned long)timeout_ms);
    return ESP_ERR_NOT_SUPPORTED;
}
