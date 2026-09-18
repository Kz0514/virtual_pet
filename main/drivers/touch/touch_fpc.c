/**
 * @file touch_fpc.c
 * @brief 12 通道 FPC 电容触摸传感器驱动（ESP32-S3, driver/touch_sens.h v2 驱动）
 *
 * 布局：
 *   左侧  (1 通道):  GPIO2  (触摸通道 2)  — 功能键
 *   顶部  (5 通道):  GPIO3-7 (触摸通道 3-7) — 水平滑块
 *   右侧  (6 通道):  GPIO8-13 (触摸通道 8-13) — 垂直滑块
 *
 * 硬件连续扫描(定时器 FSM 驱动, 每通道 50Hz) + 硬件信号链
 * (benchmark IIR / denoise 带 / smooth IIR)。**比较与去抖在软件做**
 * (软件 Schmitt + 连续帧计数, 见 TOUCH_REL_DROP_* / TOUCH_DEBOUNCE_N) —
 * 硬件 mask 只当诊断对照。息屏停软件定时器, 由独立探针任务按 esp_timer
 * 节拍探测(见 touch_fpc_sleep_probe)。触摸通道号与 GPIO 号一一对应(2-13)。
 */

#include "board.h"
#include "touch_fpc.h"
#include "driver/touch_sens.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include <string.h>
#include <math.h>

static const char *TAG = "touch_fpc";

/* v2 驱动句柄 */
static touch_sensor_handle_t  s_sens = NULL;
static touch_channel_handle_t s_chan[TOUCH_CH_COUNT] = {0};

/* 通道索引 → 触摸通道号。S3 上通道号 = GPIO 号, 故 = 索引 + 2 */
static const int s_ch_id[TOUCH_CH_COUNT] = {
    2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
};

/* 硬件去抖+滞回后的激活位图 (bit n = 通道 n)。
 * ⚠ 自 2b 起**不再参与判定**, 只在 CH0 边沿日志里打 hw= 供对账 (见扫描循环)。
 * 仍然保留中断注册的理由: 它是"硬件怎么看"的唯一观测口, 留一轮实测对比后
 * 再决定是否连同回调一起摘掉。ISR 里只赋值 — 不打印不阻塞不取 tick。
 * 回调必须 IRAM_ATTR: 本仓会在 cache 冻结期擦 flash, 届时打到 flash 里的
 * 回调即崩 (与 newlib 锁 abort / PSRAM 栈双异常同一根因族)。 */
static volatile uint32_t s_active_mask = 0;

static IRAM_ATTR bool touch_on_active(touch_sensor_handle_t sens,
                                      const touch_active_event_data_t *e, void *ctx)
{
    (void)sens; (void)ctx;
    s_active_mask = e->status_mask;
    return false;
}

static IRAM_ATTR bool touch_on_inactive(touch_sensor_handle_t sens,
                                        const touch_inactive_event_data_t *e, void *ctx)
{
    (void)sens; (void)ctx;
    s_active_mask = e->status_mask;
    return false;
}

/* 检测状态与判定不变式:
 * - touched 的唯一来源 = 软件 Schmitt (d 跨线 + 连续帧计数), 输入是硬件
 *   信号链吐出的 smooth 与 benchmark。硬件 mask 只做诊断对照
 * - bm (硬件 benchmark): 判定基准。smooth − bm 就是判定用的那个量,
 *   本文件的 d、filtered、对外的 raw/baseline 全部同源于它
 * - 软件基线链 (限速 ref / 抖动 jit / 自适应 thr) 已随 2c 删除: 亮屏判定与息屏
 *   探针现在共用同一把尺子 (smooth − 硬件 benchmark 对 active_thresh), 两组
 *   基线的量纲差异本就是"息屏/亮屏灵敏度对不上"的来源 */
typedef struct {
    uint32_t bm[TOUCH_CH_COUNT];      /* 硬件 benchmark 快照 (判定基准) */
    uint32_t raw[TOUCH_CH_COUNT];     /* 硬件 smooth 快照 (对外的 "raw") */
    bool touched[TOUCH_CH_COUNT];
    int filtered[TOUCH_CH_COUNT];
    uint32_t release_until[TOUCH_CH_COUNT]; /* 释放保活截止 (tick) — 滑动断续桥接 */
    float smooth_top_pos;
    float smooth_right_pos;
    float raw_top_pos; /* 未平滑顶部质心 — 滑动检测用(平滑系数会低估位移) */
} touch_state_t;

static touch_state_t s_ts = {0};
static TimerHandle_t s_scan_timer = NULL;

/* 实际写进驱动的硬件阈值 (init 回读确认后缓存) — touch_fpc_get_thr 用 */
static uint32_t s_thr_hw[TOUCH_CH_COUNT] = {0};

/* 标定观测: 本窗口内各通道 smooth−benchmark 峰值 (见 TUNE_PEAK_PERIOD_MS) */
static int32_t s_peak[TOUCH_CH_COUNT] = {0};
static uint32_t s_next_peak = 0; /* 下次打印峰值的时刻 (init 里置初值) */

/* ── 噪声平均数抑制 (抑制回路) ──
 * 症状: 环境近场 (手/导体停在设备左侧) 把 CH1 顶到阈值上下, 越阈 → 滑条自
 * 触发; 同时 filtered > 250 让 main.c 的 contacted 恒真 → 屏幕不熄。它不是
 * 全局噪声 (12 通道一起动), 是**局部偏置**: CH1 的 d 有近半的帧落在 0 以下,
 * 均值却明显为正, 而且是双峰不是零均值单峰 —— 有一个"抬起来就没落下"
 * 的直流分量, 那正是可以吸收掉的部分。
 *
 * 做法: 每通道维护一个慢 EMA (s_supp), 判定改用 d = draw − supp。
 *
 * ── 闸: 谁的动作不许被吸收 ──
 * 判定层已认定是动作的通道当帧就冻住 supp: 左键 (touched[0])、右滑条
 * (touched[6..11])、摸头 (pat_detector_is_active)。**被闸的动作不参与下面
 * 那条余量约束** —— 判定层翻 touched 只要 2 帧, 一个硬件样本 (60ms) 就跑完,
 * 闸合上前吸收量可忽略。所以 τ 能取到 3s 而不伤 CH0 的 10s 长按 (无闸时
 * τ=3s 会把 10s 长按的吸收量推过余量线, 有闸就不成立)。
 * 顶部滑条不能按 touched 闸 —— 近场本身就把 CH1 顶成 touched, 闸上就是永久
 * 锁死, 抑制再没有机会吸收它自己。摸头闸走的是"手指在有规律地来回动"这个
 * 语义, 与近场 (手停在旁边、单通道直流) 不重合, 所以可以加; 它同时覆盖顶条
 * 滑动 —— SWIPE_THRESHOLD 0.3 > PAT_MIN_STROKE 0.20, 任何够得上滑动的位移
 * 都已先把摸头点亮。唯一的缝在**第一笔** (摸头要等第一段行程完成才 pat_begin)
 * 与设置页 (那里 pat_detector_set_enabled(false) 主动关掉摸头, 闸跟着关) ——
 * 两处的单通道驻留都是百毫秒级, 见下。
 *
 * ── τ: 约束只剩没被闸住的顶条短动作 ──
 * travel = (S − supp)(1−e^(−T/τ)) < 余量 = 按压峰 − 阈值。
 * 实测顶条按压峰对阈值的余量约四成:
 *   顶条轻点 (tap 上限 500ms) : T=0.5  → travel 吃掉余量的 ~1/2  ✓
 *   滑动单通道驻留            : T=0.15 → travel ~1/6            ✓
 *   顶条停住 ≥1.1s            : travel 吃掉整个余量 — 但顶条没有长按 (SWIPE 在
 *                               位移当帧就发, TAP 只在 release 且 hold<500ms),
 *                               这个动作不产生任何手势, 绷紧不兑现成误判
 * τ=10s 时这三条余量大得多, 代价是**回路跟不上近场自己的脉冲**: 近场是
 * 0.2~0.8s 高/低相位交替的脉冲串, τ ≫ 脉宽时 supp 只能停在全窗均值 (DC) 上,
 * 峰的过冲原样放过去 —— 实测原始峰仍压在阈值线上, 抑制只削掉了直流那一份。
 * τ=3s 是要让 supp 跟得上单个脉冲, 把过冲
 * 也吃掉; 这一条是**本轮要验证的假设**, 不是已证结论 (若波形其实是占空比很低的
 * 稀疏脉冲, 低相位会把 supp 拽回去, τ=3s 的收益会远小于按阶跃估的值)。
 *
 * 单向不对称: "压过头"的回收不跟"升"共用曲线。近场是 20% 占空比的脉冲
 * (高相位 0.12~0.8s, 低相位更长), 回收若直接加快, supp 会在低相位里被清空
 * → supp 停在 0 而不是停在偏置上, 抑制全废 (回收时间常数至少要覆盖脉冲串
 * 低相位的典型长度才站得住)。
 * 故回收分两支:
 *   慢支 (默认): 对称 EMA, 覆盖 Δ ≤ 120 这一段 —— 它保证 supp 收敛到 mean
 *               本身, 而不是停在某个量化台阶上
 *   快支 (Δ > 120 且连续 ≥10 帧): 只有"搭手抬起"这类真事件够得着 (搭手时
 *               Δ 远超 120 且**持续**; 近场的低相位最深也只擦到 120 附近,
 *               且出现得稀疏, 凑不齐 10 连帧)。快支速率**正比于 Δ** —— 差得
 *               多就走得快, 快到位就自己慢下来, 不过头 (过头 = 抑制丢多了,
 *               滑条又自触发)。
 *
 * 尺度自洽: supp 收敛到各通道自己的均值, 各花各的余量, 不需要按通道调参。
 * 实测稳态呈左高右低的梯度 (大焊盘吸得多, 边缘通道趋近 0)。
 * **CH11 例外: 稳态吸收量长期不回落** —— 它被闸 (右滑条) 从不参与判定,
 * 所以闸合不上也退不掉, 是真偏置不是手指。量级已到该通道阈值的四成, 一旦
 * 扩散到 CH10 就会把右滑条削钝, 留观。
 *
 * 回滚杆: TOUCH_SUPP_ENABLE 置 0 → 判定退回 d = draw, 与加回路前逐位一致。 */
#define TOUCH_SUPP_ENABLE      1
#define TOUCH_SUPP_Q           256 /* 定点: 256 = 1 count (纯整数除法会把慢支截断成 0) */
#define TOUCH_SUPP_RISE_N      150 /* 慢支 N: τ = N / 50Hz = 3s (下限由顶条约束定, 见上) */
#define TOUCH_SUPP_FALL_N      50  /* 快支 N: τ = 1s (再乘 Δ 的比例 → 自限) */
#define TOUCH_SUPP_DROP_THR    120 /* 幅度门: Δ = supp − draw 超过此值才算"压过头" */
#define TOUCH_SUPP_DROP_FRAMES 10  /* 时间门: 连续满足幅度门的帧数 (0.2s @50Hz)。
                                      不能更长 —— 近场 burst 每约 0.4s 把低相位
                                      打断一次, 门比它长就永远触发不了 */
static int32_t s_supp_q8[TOUCH_CH_COUNT] = {0};  /* 已吸收的偏置 (Q8 定点, ≥0) */
static uint8_t s_drop_run[TOUCH_CH_COUNT] = {0}; /* 快支时间门计数 */
/* 摸头闸 (pat_detector 置位, 见上「闸」段)。volatile: 写方在 LVGL 任务
 * (pat_detector 的 20ms 轮询), 读方在扫描定时器上下文, 两边都不持锁 */
static volatile bool s_supp_gate_pat = false;

/* ── 判定: 软件 Schmitt + 连续帧去抖 ──
 * 输入仍是硬件濾波后的 smooth 与 benchmark (IIR_2 / IIR_16 + denoise 带),
 * 只是把最后一步"比较 + 去抖"从硬件拿回软件。原因 (2b 实测):
 * 硬件释放下限约 100ms —— 手指已离开、d 早已低于硬件释放线, 仍不报释放,
 * 两次点击被合并成一次按压 → 单击被吞成长按、双击退化。
 *   起按线 = 硬件 active_thresh (按通道给), 与硬件同一把尺;
 *     单帧尖峰凑不齐连续 N 帧, 误激活仍然防住 (插线瞬态单帧越线不成立)。
 *   释放线 = 起按线 − 该通道滞回量: 实测真抬起时 d 砸得比"指腹在键上滚动"
 *     的谷底更低 —— 300 落在两者之间, 既不吞真释放也不把滚动当抬起。
 * 帧间隔: 软件扫描 20ms, 但**硬件每通道 60ms 才出一个新样本** (逐帧轨迹同值帧数
 * 中位 = 3, 同一读数被读了三次) —— 所以本计数器实际只隔一个硬件样本就够 N 帧,
 * 真正的单帧瞬态防线是硬件 debounce_cnt + 阈值, 不是这里。 */
#define TOUCH_REL_DROP_CH0 100 /* 起按线 400 − 100 = 释放线 300: 下探起按端, 释放端不动 */
#define TOUCH_REL_DROP_BAR 150 /* 起按线 350 − 150 = 释放线 200 (滑条本轮未动) */
#define TOUCH_DEBOUNCE_N   2   /* 连续帧数到 N 翻按下, 到 0 翻释放 */
static uint8_t s_deb[TOUCH_CH_COUNT] = {0};

/* ── 硬件判定阈值 (active_thresh, 单位 counts) ──
 * 语义 = smooth − benchmark 超过此值即判激活; 释放线 = 阈值 − active_hysteresis。
 * 20s 峰值窗口实测 (smooth−benchmark, 连未激活的帧也计入) 的相对量级:
 *   CH0: 正常按压 ≈ 阈值的 4~5 倍 | 快速轻点峰 ≈ 3.6 倍
 *        空载峰 ≈ 阈值的 1/4 | 插线瞬态单帧 ≈ 3 倍
 *   CH1: 真实触摸 ≈ 阈值的 1.5 倍 | 空载峰高达阈值的 0.7~0.9 倍
 * 两道防线分工明确: 阈值挡"幅度不够"的, debounce_cnt 挡"只有一帧"的 ——
 * 插线瞬态峰值明明高于阈值却未激活, 正是因为它只维持一帧。
 * 故阈值不必贴着幻触定: CH0 取 400, 滑条取 350 (单值阈值会把整条右侧滑条打死,
 * 必须按通道给)。
 * CH0 450→400: 逐帧轨迹里按得轻的那几次峰值对 450 只有 1.7~2.6 倍 ——
 *   再轻再快就有整个峰摸不到线的风险 (手指能否被采到还取决于硬件 60ms 一巡的
 *   占空, 阈值下探只能救"采到了但没过线"那一半)。降后空载峰仍有近 3 倍余量。
 *   滞回量同步 150→100, 释放线**仍是 300, 释放端逐位不变** (见 TOUCH_REL_DROP_CH0)。
 * ⚠ 让出的唯一窗口: 插线瞬态若升级成"连续 2 个硬件样本都在 400~450"就会立案。
 *   实测未见 (瞬态仍是单样本), 待观测。
 * ⚠ 滑条余量偏紧: CH1 空载峰距阈值不足 20% —— 实测若出现滑条自触发,
 *   第一个要抬的就是这里。 */
#define TOUCH_THR_CH0 400            /* 左键 (CH0/GPIO2, 最大焊盘, USB 耦合最强) */
#define TOUCH_THR_BAR 350            /* 顶部 CH1-5 与右侧 CH6-11 */

#define RELEASE_KEEP_MS 60           /* 释放保活: 刚释放 60ms 内仍计 touched —
                                        桥接滑动腾空间隙/压力波动 (按 tick,
                                        50Hz=3帧 / 20Hz 空闲档同样生效)。
                                        只作用于滑条通道 (CH1-11), 左键不保活
                                        (保活拖长单击 hold → 500ms 轻点边界误判长按) */
#define ENV_SNAP_CHANNELS 11         /* 环境阶跃: >=11 通道同时激活 → 清判定 + 2s 免疫窗。
                                       手掌覆盖 ≤10 通道不误伤。软件基线快照已随 2c 删,
                                       但清 touched 与免疫窗仍是活的 (探针直读免疫窗) */
/* ── 标定观测 (临时仪表, 定标收敛后随 2c 一起删) ──
 * 判定切硬件后, 误触发不再产生任何日志 —— 幻触消失的直接表现就是"什么都没发生",
 * 但同时也看不到"离阈值还差多远"。每 20s 打一窗口内 12 通道 smooth−benchmark
 * 峰值, 作为调 active_thresh 的唯一仪表 (对着 TOUCH_THR_* 看余量)。 */
#define TUNE_PEAK_PERIOD_MS 20000

/* ── 刷新率 ──
 * 亮屏恒 50Hz (软件定时器, 与硬件连续扫描同节拍)。
 * 息屏两档由探针自己的 esp_timer 节拍承担, 与亮屏无关:
 * - 息屏快探 20Hz: 100ms 轻掠需 ≥2 采样帧凑齐 2 连击去抖
 * - 息屏深闲 2Hz: 无活动 15s → 睡眠窗口 500ms, 主功耗收益点
 * 亮屏侧原有"空闲 20Hz"一档已随 2c 删除: 它改不到硬件扫描周期
 * (meas_interval_us 运行期不可改), 省不到功耗大头 (亮屏持 PM 锁本就不睡),
 * 却把去抖时间 (TOUCH_DEBOUNCE_N 帧) 与 filtered IIR 时间常数 (4 帧) 绑在
 * "用户多久没摸"上 —— 空闲 30s 后按压迟滞 40→100ms、滑条跟手钝 2.5x。
 * 活动判定: 任一通道 |d| > 80 (近场/手接近) → touch_note_activity */
#define PROBE_ACT_THR 80             /* 活动判定阈值 (近场/手接近) */
#define SCAN_ON_MS 20                /* 亮屏扫描节拍 50Hz */
#define PROBE_FAST_MS 50             /* 息屏快探 20Hz */
#define PROBE_SLOW_MS 500            /* 息屏深闲 2Hz */
#define PROBE_SLOW_AFTER_MS 15000    /* 息屏无活动多久降档 */

static uint32_t s_last_activity = 0;       /* 最近活动时刻 (tick) */
static uint32_t s_probe_interval = PROBE_FAST_MS; /* 当前息屏探针间隔 */

/* 独立探针任务: 息屏期由周期 esp_timer (RTC 闹钟) 驱动 —
 * 硬性绑定轻睡唤醒节拍, 探针频率 = probe_interval, 与主循环负载解耦 */
static TaskHandle_t s_probe_task = NULL;
static esp_timer_handle_t s_probe_timer = NULL;
static volatile bool s_probe_enabled = false;
static volatile bool s_wake_pending = false;
/* 互斥 oneshot 扫描与 start/stop continuous —— 必须的, 不是保险:
 * trigger_oneshot_scanning 全程把 is_started 置 true (驱动
 * touch_sens_common.c:373 置位 / :422 复位), 而 start_continuous_scanning
 * 要求 !is_started (:323)。息屏转亮屏时探针任务多半正卡在 oneshot 里, 直接
 * start 会 ESP_ERR_INVALID_STATE —— 实测 100% 复现, 后果是唤醒后硬件扫描
 * 再也没起来 (亮屏全程摸不动, 峰值读数冻结) */
static SemaphoreHandle_t s_probe_mtx = NULL;

static void touch_note_activity(void)
{
    s_last_activity = xTaskGetTickCount();
    s_probe_interval = PROBE_FAST_MS;
}

/** 当前息屏探针间隔 (main.c 据此调循环延迟 — 深闲时睡眠窗口 500ms) */
uint32_t touch_fpc_probe_interval_ms(void) { return s_probe_interval; }

/** 诊断: 各通道真实判定阈值 (硬件 active_thresh, init 回读缓存) — 排障看死区 */
void touch_fpc_get_thr(int *thr_out)
{
    for (int i = 0; i < TOUCH_CH_COUNT; i++)
        thr_out[i] = (int)s_thr_hw[i];
}

/* ── 硬件基准读数 (诊断仪表) ──
 * 只读 + 打印, 不写任何状态、不参与任何判定。
 * 注意打印的是**直读值**: 2c 之前这里先把 benchmark 抄进软件 ref 再打印, 被防手指
 * 守卫跳过时打出来的其实是上一轮的旧值 —— 会把"基准跑了"误读成"基准正常" */
static void touch_log_hw_baseline(void)
{
    uint32_t bm[TOUCH_CH_COUNT] = {0};
    uint32_t chk = 0, vmax = 0;
    for (int i = 0; i < TOUCH_CH_COUNT; i++) {
        uint32_t v = 0;
        if (touch_channel_read_data(s_chan[i], TOUCH_CHAN_DATA_TYPE_BENCHMARK, &v) == ESP_OK)
            bm[i] = v;
        chk += v;
        if (v > vmax) vmax = v;
    }
    /* 两种"benchmark 不是数"的情形: 全 0 / 满量程 (0x3FFFFF, 22 位, 复位后尚未
     * 落定, 见 touch_fpc_init 的注释)。只标注不吞掉 —— 仪表就该显示实况 */
    if (chk == 0 || vmax == 0x3FFFFFu)
        ESP_LOGW(TAG, "基准读数异常 (全 0 或满量程 %u)", (unsigned)vmax);

    /* 这 12 个数就是判定基准本身 (硬件 benchmark), 各通道之间应大致同量级 —
     * 某个明显偏低即是"基准没爬到位"/"上面有东西" */
    ESP_LOGI(TAG, "硬件基准: CH0=%ld CH1=%ld CH2=%ld CH3=%ld CH4=%ld CH5=%ld "
                  "CH6=%ld CH7=%ld CH8=%ld CH9=%ld CH10=%ld CH11=%ld",
             (long)bm[0], (long)bm[1], (long)bm[2], (long)bm[3],
             (long)bm[4], (long)bm[5], (long)bm[6], (long)bm[7],
             (long)bm[8], (long)bm[9], (long)bm[10], (long)bm[11]);
}

/* ── 息屏唤醒探针 (独立任务 + esp_timer 节拍) ──
 * 探针由周期 esp_timer (RTC 闹钟) 驱动 — 频率精确 = probe_interval
 * (快 50ms / 深闲 500ms), 与主循环负载解耦。
 * 连击判定: 2 命中须落 150ms 窗内且同通道 — 真按 ≥100ms 必 2-3 连中;
 * 漂移越阈命中相隔秒级, 凑不齐连击。
 * 免疫窗: USB/供电事件后 5s 不判唤醒; 通道数 >10 判环境态。 */
static uint8_t s_probe_hits = 0;
static uint32_t s_hit_tick = 0;   /* 末次命中时刻 — 连击 150ms 时序窗判据 */
static int8_t s_hit_ch = -1;      /* 末次命中主通道 — 连击必须同通道 */
static int8_t s_act_ch = -1;      /* 息屏活动主通道累积: 同通道 2 帧才活动 */
static TickType_t s_immune_until = 0; /* USB/供电事件免疫窗 */
static bool s_oneshot_failed = false; /* oneshot 失败沿 — 只报一次, 免 20Hz 刷屏 */
#define PROBE_HIT_REQUIRED 2
#define PROBE_HIT_WINDOW_MS 150   /* 两命中必须在此窗内 — 漂移越阈(间隔秒级)凑不齐 */
#define PROBE_MAX_CHANNELS 10

/** 摸头活动状态 (pat_detector 在进入/退出摸头时通知) — 顶条往返滑动期间
 * 冻住抑制回路。app → driver 单向通知, 避免 driver #include app 的反向依赖。 */
void touch_fpc_set_pat_active(bool on) { s_supp_gate_pat = on; }

/* 判定用 d = smooth − benchmark − 已吸收偏置。亮屏判定 / 息屏探针复判 / 抑制
 * 回路必须共用同一把尺子 —— 两边各写一份是"息屏与亮屏灵敏度对不上"的老病根。
 * 息屏期 s_supp_q8 是冻结的 (回路只走亮屏路径推它), 即"入睡那一刻吸收了多少
 * 就扣多少" —— 近场偏置不会因为睡着就假装不存在, 也不会在睡眠里被继续吃掉。
 * ⚠ 扫描循环里是展开写的 (它还要拿 draw 和 supp 各自去推回路), 改这里要同步改那里 */
static inline int32_t touch_d_supp(int i)
{
    return (int32_t)s_ts.raw[i] - (int32_t)s_ts.bm[i] - s_supp_q8[i] / TOUCH_SUPP_Q;
}

/* ── 单次扫描 (亮屏定时器回调与息屏探针共用; 亮屏固定 50Hz, 息屏由探针自定节拍) ──
 * sleep_path: 息屏探针路径 — 该路径不落盘判定 (见下方 s_deb 归零),
 * 唤醒与否全由探针独立复判 */
static void touch_scan_once(bool sleep_path)
{
    uint32_t now = xTaskGetTickCount();
    int n_dev = 0;
    bool near = false;

    /* 抑制闸: 判定层已认定是动作的通道不让回路吸收 (理由见 TOUCH_SUPP_* 注释块)。
     * 取的是**上一帧**的 touched —— 跨 12 通道用同一份快照, 免得循环走到一半
     * 混进本帧刚翻过来的状态; 20ms 陈旧对本回路无影响 */
    bool supp_gate = s_supp_gate_pat || s_ts.touched[0];
    for (int i = TOUCH_TOP_CH_COUNT + 1; i < TOUCH_CH_COUNT; i++)
        supp_gate |= s_ts.touched[i];
    /* 息屏期判定不落盘 — 唤醒判定由探针独立复判 (smooth−benchmark 对
     * active_thresh, 无保活) 承担, scan 侧只维护 touched/filtered/质心。
     * 免疫窗由探针端裁决 (touch_fpc_sleep_probe), 扫描侧不施加窗口 */

    int8_t act_ch = -1; /* 息屏活动判定: |d| 最大通道 (跨 2 帧必须同通道) */
    int32_t act_d = -1;

    for (int i = 0; i < TOUCH_CH_COUNT; i++) {
        /* 数据源 = 硬件 smooth 与硬件 benchmark (IIR_2 / IIR_16 已滤过),
         * 同一寄存器 touch_pad_data 换 sel 读, 同量纲。读失败沿用上帧值 —
         * 填 0 会造出巨大负 delta → 误判释放 */
        uint32_t sm = s_ts.raw[i];
        uint32_t bm = s_ts.bm[i];
        if (touch_channel_read_data(s_chan[i], TOUCH_CHAN_DATA_TYPE_SMOOTH, &sm) == ESP_OK)
            s_ts.raw[i] = sm;
        if (touch_channel_read_data(s_chan[i], TOUCH_CHAN_DATA_TYPE_BENCHMARK, &bm) == ESP_OK)
            s_ts.bm[i] = bm;

        /* draw = smooth − benchmark — 与硬件判定 (active_thresh) 同量纲同符号。
         * ESP32-S3: 触摸时该值上升 */
        int32_t draw = (int32_t)s_ts.raw[i] - (int32_t)s_ts.bm[i];

        /* ── 抑制回路 (见 TOUCH_SUPP_* 注释块) ──
         * 喂进去的是**未抑制的** draw: 喂 d 会让 supp 收敛到 mean/2 (自反馈
         * 每轮再砍一半)。只走亮屏路径 —— 息屏探针 2~20Hz, 时间常数会和亮屏
         * 差一个量级, 两条路径共用 s_supp 但只有亮屏推它 */
        int32_t supp = s_supp_q8[i] / TOUCH_SUPP_Q;
#if TOUCH_SUPP_ENABLE
        if (!sleep_path && !supp_gate) {
            int32_t delta = supp - draw; /* >0 = 压过头了 */
            if (delta <= TOUCH_SUPP_DROP_THR) {
                /* 慢支: 对称 EMA, Δ ≤ 120 全走这里 (含升与慢降) */
                if (s_drop_run[i]) s_drop_run[i] = 0;
                s_supp_q8[i] += (draw * TOUCH_SUPP_Q - s_supp_q8[i]) / TOUCH_SUPP_RISE_N;
            } else {
                /* 计数器封顶只封自己, 不封触发 —— 封在一起的话持续 >5s 的
                 * 过压会在第 255 帧后静默停止回收 */
                if (s_drop_run[i] < 255) s_drop_run[i]++;
                if (s_drop_run[i] >= TOUCH_SUPP_DROP_FRAMES) {
                    /* 快支: 幅度门 + 时间门都过 → 正比于 Δ 地还回去 */
                    s_supp_q8[i] -= delta * TOUCH_SUPP_Q / TOUCH_SUPP_FALL_N;
                }
            }
            if (s_supp_q8[i] < 0) s_supp_q8[i] = 0;
            /* 上限 = 该通道阈值: 吸收量超过阈值等于把该通道打死 */
            else if (s_supp_q8[i] > (int32_t)s_thr_hw[i] * TOUCH_SUPP_Q)
                s_supp_q8[i] = (int32_t)s_thr_hw[i] * TOUCH_SUPP_Q;
        }
#endif
        int32_t d = draw - supp;

        if (!sleep_path && d > s_peak[i])
            s_peak[i] = d; /* 标定仪表 (TUNE_PEAK_PERIOD_MS) — 记的是抑制后的 d */

        bool was = s_ts.touched[i];
        /* 判定 = 软件 Schmitt + 连续帧去抖 (见 TOUCH_REL_DROP_* / TOUCH_DEBOUNCE_N)。
         * 计数器到 N 才翻按下, 掉到 0 才翻释放 —— 单帧摆动两个方向都过不去。
         * 按下态用释放线, 释放态用起按线, 中间那条带 (CH0 100 / 滑条 150) 是滞回。
         * 硬件 mask 只用于诊断对照: 它与软件判定的分歧本身就是要观测的东西 */
        bool hw = (s_active_mask & (1u << s_ch_id[i])) != 0;
        bool active = false;
        if (sleep_path) {
            /* 息屏期不落盘判定 (唤醒由探针独立复判), 且**必须把计数器归零**:
             * 否则 2~4s 一波的供电瞬态足以把某通道顶到 N, 唤醒首帧 was=false
             * 而 s_deb>=N → 凭空一枚按下。归零的代价只是唤醒后头 40ms 不响应 */
            s_deb[i] = 0;
        } else {
            int32_t rel = (i == 0) ? TOUCH_REL_DROP_CH0 : TOUCH_REL_DROP_BAR;
            int32_t line = was ? (int32_t)s_thr_hw[i] - rel : (int32_t)s_thr_hw[i];
            if (d > line) {
                if (s_deb[i] < TOUCH_DEBOUNCE_N)
                    s_deb[i]++;
            } else if (s_deb[i] > 0) {
                s_deb[i]--;
            }
            active = was ? (s_deb[i] > 0) : (s_deb[i] >= TOUCH_DEBOUNCE_N);
        }

        /* 功能键状态翻转诊断: 附 smooth/benchmark 对账, hw= 是硬件 mask 的同拍值
         * (息屏探针不落盘, 不打印) */
        if (i == 0 && !sleep_path && (active != was))
            ESP_LOGI(TAG, "CH0 %s: d=%ld (raw=%ld sm=%lu bm=%ld) thr=%u hw=%d",
                     active ? "按下" : "释放", (long)d, (long)draw,
                     (unsigned long)s_ts.raw[i], (long)s_ts.bm[i],
                     (unsigned)s_thr_hw[0], (int)hw);

        if (active) {
            s_ts.touched[i] = true;
            n_dev++;
            s_ts.release_until[i] = now + pdMS_TO_TICKS(RELEASE_KEEP_MS); /* 每次激活续期保活 */
        } else {
            /* 释放保活: 滑条通道最近激活后 60ms 内仍维持 touched — 桥接滑动中
             * 手指腾空换电极/压力瞬时跌破滞回阈的间隙, 防质心骤失断触; 手指
             * 真离开 60ms 内释放, 迟滞无感。保活帧不计 n_dev (手掌拿开瞬间
             * 12 通道同归释放, 计数会误判为环境阶跃触发快照) */
            bool keep = (i > 0) && was && (int32_t)(now - s_ts.release_until[i]) < 0;
            s_ts.touched[i] = keep;
            if (keep)
                continue; /* 保活帧: filtered/质心权重沿用离开前值 → 位置保持该通道
                              (保活期手指已在途中, 刷新会稀释权重导致质心漂移) */
        }
        s_ts.filtered[i] = (s_ts.filtered[i] * 3 + (d > 0 ? (int)d : 0)) / 4;

        /* 活动判定 (近场/手接近, 含按压前的近场信号): |d|>80 → 刷新活动
         * 时刻 → 动态刷新率立即回最高档。息屏 (sleep_path) 改同通道连续
         * 2 帧: 供电瞬态 (2-4s 一波通道间轮换) 单帧即越阈, 单帧计活动会
         * 持续续活使深闲档不可达; 瞬态轮换凑不齐同通道 2 帧, 真按必齐。
         * 亮屏路径保持单帧 (活动推进动画帧率) */
        int32_t ad = (d > 0) ? d : -d;
        if (ad > PROBE_ACT_THR) {
            if (!sleep_path)
                near = true;
            else if (ad > act_d) {
                act_d = ad;
                act_ch = (int8_t)i;
            }
        }
    }
    if (sleep_path) {
        if (act_ch >= 0 && act_ch == s_act_ch) {
            touch_note_activity();
            s_act_ch = -1; /* 已刷新 — 重累积; 持续按压仍每 2 帧续活 */
        } else {
            s_act_ch = act_ch;
        }
    } else if (near) {
        touch_note_activity();
    }

    /* 环境阶跃: >=11 通道同时激活 = 手掌全覆盖/供电瞬态, 非单点触摸 —
     * 清判定 + 2s 免疫窗。手掌覆盖 ≤10 通道不误伤。
     * 注意: 这里清 touched 只是让质心当帧归零 (下一帧由软件 Schmitt 重新给出),
     * 真正的幻触防线是 CH0 的 active_thresh 与 debounce_cnt */
    if (n_dev >= ENV_SNAP_CHANNELS) {
        for (int i = 0; i < TOUCH_CH_COUNT; i++)
            s_ts.touched[i] = false;
        s_immune_until = now + pdMS_TO_TICKS(2000);
        touch_note_activity();
    }

    /* 顶部滑块（通道 1-5）：加权质心 → -1..+1 */
    float t_w = 0, t_s = 0;
    for (int i = 1; i <= TOUCH_TOP_CH_COUNT; i++) {
        if (s_ts.touched[i]) {
            float w = (float)s_ts.filtered[i];
            t_w += w * (i - 1);
            t_s += w;
        }
    }
    float tp = (t_s > 0) ? (t_w / t_s) / (TOUCH_TOP_CH_COUNT - 1) : -1.0f;
    tp = tp * 2.0f - 1.0f;
    s_ts.raw_top_pos = tp;
    s_ts.smooth_top_pos = s_ts.smooth_top_pos * 0.7f + tp * 0.3f;

    /* 右侧滑块（通道 6-11）：加权质心 → -1..+1 */
    float r_w = 0, r_s = 0;
    for (int i = TOUCH_TOP_CH_COUNT + 1; i < TOUCH_CH_COUNT; i++) {
        if (s_ts.touched[i]) {
            float w = (float)s_ts.filtered[i];
            r_w += w * (i - TOUCH_TOP_CH_COUNT - 1);
            r_s += w;
        }
    }
    float rp = (r_s > 0) ? (r_w / r_s) / (TOUCH_RIGHT_CH_COUNT - 1) : -1.0f;
    rp = rp * 2.0f - 1.0f;
    s_ts.smooth_right_pos = s_ts.smooth_right_pos * 0.3f + rp * 0.7f; /* faster response */
}

/* ── 息屏期唤醒探针 ──
 * 每次先触发一轮 oneshot 扫描取数, 连击判定独立复判 (逐通道
 * smooth − benchmark > active_thresh, 无滞回无保活); USB/供电瞬态由
 * 免疫窗 (5s) 覆盖, scan 侧环境快照服务亮屏期瞬态 */

bool touch_fpc_sleep_probe(void)
{
    /* 息屏期硬件连续扫描已停 (见 touch_fpc_pause) — 先触发一轮 oneshot 把 12
     * 通道各测一次, 再让 touch_scan_once 去读结果。这是息屏功耗的主要杠杆:
     * 连续扫描是 12 通道每 ~60ms 一巡且常开, 这里改成按档位 (快探 / 深闲)
     * 一巡。硬件 benchmark 在 oneshot 下同样自追踪 → 漂移补偿不用另写。
     * ⚠ 它是**逐通道阻塞**的 (每通道 vTaskDelay(1) 等本通道测完) —— 只能在
     *   任务上下文调。本函数唯一调用点是 probe_task_fn, 满足; 绝不可从
     *   LVGL 定时器 / esp_timer 回调 / ISR 进。
     * timeout 传 200: 驱动里先判 `timeout_ms > 0` 才换算 tick, 传 0 会
     *   end_tick = now 当场超时 (负值才是不限时); 200ms 对 12 通道绰绰有余。 */
    esp_err_t os_err = touch_sensor_trigger_oneshot_scanning(s_sens, 200);
    if (os_err != ESP_OK) {
        if (!s_oneshot_failed) { /* 失败沿只报一次 */
            s_oneshot_failed = true;
            ESP_LOGW(TAG, "oneshot 扫描失败 (%s) — 探针读数停在上轮, 唤醒会变迟钝",
                     esp_err_to_name(os_err));
        }
    } else {
        s_oneshot_failed = false;
    }

    touch_scan_once(true); /* 息屏路径: 追速 ×2 盖过 PA 关/面睡漂移源 */

    /* 动态刷新率 (息屏档): 距上次活动 >15s → 深闲 2Hz (睡眠窗口
     * 50→500ms, 主功耗收益); 任何活动 (近场/触摸/免疫事件) 已由
     * touch_scan_once 的 touch_note_activity 复位为快探 20Hz */
    if ((xTaskGetTickCount() - s_last_activity) * portTICK_PERIOD_MS >
        PROBE_SLOW_AFTER_MS)
        s_probe_interval = PROBE_SLOW_MS;
    else
        s_probe_interval = PROBE_FAST_MS;

    /* 免疫窗: USB 拔插/模式切换后 5s 内不判唤醒 — 瞬态 (VBUS
     * 消失/恢复, PHY 电源域切换) 使 raw 持续跳变 1-2s。
     * 手动唤醒路径独立, 不在此窗内 */
    if (xTaskGetTickCount() < s_immune_until) {
        s_probe_hits = 0;
        return false;
    }

    /* 连击判定独立复判 d > active_thresh, 不用 s_ts.touched — touched 含 60ms
     * 释放保活, 会把单帧环境尖峰撑成多帧计数在窗内凑齐连击 → 假唤醒。
     * 独立复判 (无滞回无保活): 尖峰单帧越阈只计 1 次, 真实按压 ≥100ms
     * 在快探档下 ≥2 帧连续越阈必齐。通道数沿用 PROBE_MAX_CHANNELS
     * (手掌覆盖整体越阈不判)。
     * 尺子与亮屏判定同一把 (smooth − benchmark 对 s_thr_hw), 不再另起软件基线 —
     * 两组基线的量纲差异本身就是息屏/亮屏灵敏度对不上的来源 */
    int n_act = 0;
    for (int i = 0; i < TOUCH_CH_COUNT; i++) {
        if (touch_d_supp(i) > (int32_t)s_thr_hw[i])
            n_act++;
    }
    if (n_act == 0 || n_act > PROBE_MAX_CHANNELS) {
        s_probe_hits = 0;
        s_hit_ch = -1;
        return false;
    }

    /* 连击必须同通道: 供电瞬态跨通道轮换, 窗内异通道命中作废; 真实
     * 按压固定同通道, 不受影响。主通道 = 超阈通道中 d 最大者 */
    int8_t best_ch = -1;
    int32_t best_d = -INT32_MAX;
    for (int i = 0; i < TOUCH_CH_COUNT; i++) {
        int32_t d = touch_d_supp(i);
        if (d > (int32_t)s_thr_hw[i] && d > best_d) {
            best_d = d;
            best_ch = (int8_t)i;
        }
    }

    /* 150ms 时序窗 + 通道一致性: 两命中相隔 >150ms 或换通道 → 重新起算
     * (漂移/瞬态轮换凑不齐); 真按 50ms 节拍同通道必齐 */
    uint32_t now_t = xTaskGetTickCount();
    if (s_probe_hits &&
        ((now_t - s_hit_tick) > pdMS_TO_TICKS(PROBE_HIT_WINDOW_MS) ||
         best_ch != s_hit_ch))
        s_probe_hits = 0;
    s_probe_hits++;
    s_hit_tick = now_t;
    s_hit_ch = best_ch;
    if (s_probe_hits < PROBE_HIT_REQUIRED)
        return false;

    s_probe_hits = 0;
    s_wake_pending = true; /* 唤醒标志 → main.c 消费 (屏幕状态/日志一体化) */
    return true;
}

/* 独立探针任务 + esp_timer 节拍:
 * esp_timer 是 RTC 闹钟: 周期 alarm 硬性约束轻睡唤醒节拍, 探针频率
 * 精确 = 配置值, 与主循环负载无关 (vTaskDelay/主循环阻塞拖不动它)。
 * 任务仅在息屏期 (s_probe_enabled) 运行探测, 亮屏期阻塞在通知上, 零开销。 */
static void probe_timer_cb(void *arg)
{
    BaseType_t hp = pdFALSE;
    xTaskNotifyFromISR(s_probe_task, 0, eNoAction, &hp);
    portYIELD_FROM_ISR(hp);
}

static void probe_task_fn(void *arg)
{
    uint32_t cur_iv_us = 0;
    for (;;) {
        /* 亮屏期: 等待使能 (pause → xTaskNotifyGive) */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (!s_probe_enabled) { /* pause 清标志后兜底再等 */
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            if (!s_probe_enabled) continue;
        }

        cur_iv_us = PROBE_FAST_MS * 1000;
        esp_timer_start_periodic(s_probe_timer, cur_iv_us);

        while (s_probe_enabled) {
            BaseType_t rc = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(600));
            (void)rc;
            if (!s_probe_enabled) break;
            xSemaphoreTake(s_probe_mtx, portMAX_DELAY);
            /* 取锁后复检: resume 可能在我们等锁的这几微秒里把标志清了, 那时
             * 硬件连续扫描已经起来, 再跑 oneshot 只会撞 INVALID_STATE */
            if (s_probe_enabled)
                touch_fpc_sleep_probe(); /* 内部按活动/深闲更新 s_probe_interval */
            xSemaphoreGive(s_probe_mtx);
            uint32_t iv = s_probe_interval * 1000;
            if (iv != cur_iv_us) {  /* 快探↔深闲 档位切换 → 重设周期 */
                esp_timer_stop(s_probe_timer);
                esp_timer_start_periodic(s_probe_timer, iv);
                cur_iv_us = iv;
            }
        }
        esp_timer_stop(s_probe_timer);
        cur_iv_us = 0;
    }
}

/** main.c 消费: 探针 2 连击达成 → 置唤醒位, 主循环据此亮屏 (返回即清除) */
bool touch_fpc_wake_pending(void)
{
    if (!s_wake_pending) return false;
    s_wake_pending = false;
    return true;
}

/* USB/供电事件 → 5s 免疫窗 (见 touch_fpc.h 注释) */
void touch_fpc_note_usb_event(void)
{
    s_immune_until = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
    s_probe_hits = 0;
    touch_note_activity(); /* 免疫期仍保持快探档 (瞬态结束后立即恢复响应) */
}

/* 诊断: 去抖命中数 — power_log.csv ph 列 */
uint8_t touch_fpc_probe_hits(void) { return s_probe_hits; }

/* 诊断: 当前超阈值通道数 — power_log.csv tc 列 */
uint8_t touch_fpc_touched_count(void)
{
    uint8_t n = 0;
    for (int i = 0; i < TOUCH_CH_COUNT; i++)
        if (s_ts.touched[i]) n++;
    return n;
}

/* ── 周期性扫描定时器回调 (亮屏动态刷新率: 活动 50Hz / 空闲 20Hz) ──
 * 手势窗口余量: 20Hz 下数据最多陈旧 50ms — 释放边滞后 ≤50ms, 远小于
 * 最小手势窗口 (NAV_TAP_MAX_MS=500ms / NAV_SETTLE_MS=100ms); 触摸起始
 * 边活动判定 ≤50ms 内回 50Hz。LVGL indev 轮询只读最新状态, 无影响。
 * 亮屏持 PM 锁禁睡, 此档只省定时器唤醒 + 扫描 CPU — 适中即可 */
static void touch_scan_timer_cb(TimerHandle_t timer)
{
    touch_scan_once(false); /* 亮屏路径: 追速 200/s, 保慢滑余量 */

    /* 标定仪表 (TUNE_PEAK_PERIOD_MS): 本窗口 12 通道 smooth−benchmark 峰值。
     * 对着 TOUCH_THR_* 看余量 — 峰值应稳定低于阈值; 若某个通道常态贴着阈值,
     * 说明它离误触发只差一次扰动, 该抬阈 (或该查那块焊盘) */
    {
        uint32_t now_t = xTaskGetTickCount();
        if ((int32_t)(now_t - s_next_peak) >= 0) {
            s_next_peak = now_t + pdMS_TO_TICKS(TUNE_PEAK_PERIOD_MS);
            ESP_LOGI(TAG, "峰值: %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld",
                     (long)s_peak[0], (long)s_peak[1], (long)s_peak[2], (long)s_peak[3],
                     (long)s_peak[4], (long)s_peak[5], (long)s_peak[6], (long)s_peak[7],
                     (long)s_peak[8], (long)s_peak[9], (long)s_peak[10], (long)s_peak[11]);
            memset(s_peak, 0, sizeof(s_peak));

#if TOUCH_SUPP_ENABLE
            /* 抑制量本身 — 回路的**唯一**存在证明。它若贴在 0 附近, 说明回路
             * 压根没吸进去 (而"峰值:"看起来正常只是因为本来就没超阈) */
            ESP_LOGI(TAG, "均值: %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld",
                     (long)(s_supp_q8[0] / TOUCH_SUPP_Q), (long)(s_supp_q8[1] / TOUCH_SUPP_Q),
                     (long)(s_supp_q8[2] / TOUCH_SUPP_Q), (long)(s_supp_q8[3] / TOUCH_SUPP_Q),
                     (long)(s_supp_q8[4] / TOUCH_SUPP_Q), (long)(s_supp_q8[5] / TOUCH_SUPP_Q),
                     (long)(s_supp_q8[6] / TOUCH_SUPP_Q), (long)(s_supp_q8[7] / TOUCH_SUPP_Q),
                     (long)(s_supp_q8[8] / TOUCH_SUPP_Q), (long)(s_supp_q8[9] / TOUCH_SUPP_Q),
                     (long)(s_supp_q8[10] / TOUCH_SUPP_Q), (long)(s_supp_q8[11] / TOUCH_SUPP_Q));
#endif
        }
    }

}

/* ── 公开 API ── */
esp_err_t touch_fpc_init(void)
{
    ESP_LOGI(TAG, "正在初始化 12 通道 FPC 触摸传感器（v2 驱动）…");

    /* 轻睡必须显式保触摸域: legacy 靠 FSM 定时器恒开把 RTC_PERIPH 隐式保电
     * (sleep_modes.c 读 touch_slp_timer_en 决定 keep_rtc_power_on), 这条隐式
     * 依赖随 legacy 一起没了 → 轻睡会清掉触摸寄存器/benchmark (官方注释原文:
     * "otherwise the touch sensor FSM will be cleared, causing touch sensor
     * false triggering")。只补电源域, 不调 touch_sensor_config_sleep_wakeup —
     * 那个顺带开硬件触摸唤醒 (RTC_TOUCH_TRIG_EN), 与软件探针架构冲突。 */
    ESP_ERROR_CHECK(esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON));

    /* 采样参数对齐 legacy 默认 (S3: TOUCH_PAD_MEASURE_CYCLE_DEFAULT=500 /
     * 2V7 / 0V5 / idle 接 GND) → 读数尺度与迁移前逐计数可比 */
    static touch_sensor_sample_config_t s_sample_cfg[TOUCH_SAMPLE_CFG_NUM] = {
        TOUCH_SENSOR_V2_DEFAULT_SAMPLE_CONFIG(500, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_2V7)
    };
    touch_sensor_config_t sens_cfg = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(1, s_sample_cfg);
    /* meas_interval_us 只是**下限**: 12 通道 × 1.667ms = 20ms/轮。实测硬件每通道
     * 60ms 才出一个新样本 (逐帧轨迹同值帧数中位 = 3, 而软件扫描 20ms) —— 即每通道
     * 测量本身要约 5ms, 远大于 1.667ms, 间隔被测量时间吃掉, 实际轮期 ≈ 60ms */
    sens_cfg.meas_interval_us = 1667;
    ESP_ERROR_CHECK(touch_sensor_new_controller(&sens_cfg, &s_sens));

    /* 12 个通道: 通道号 = GPIO 号 (S3), 见 s_ch_id。
     * active_thresh 就是判定阈值 (见 TOUCH_THR_* 的分档依据), 按通道给 —
     * 右侧滑条按压幅度只有 CH0 的一半, 单一阈值会把整条右侧滑条打死 */
    for (int i = 0; i < TOUCH_CH_COUNT; i++) {
        touch_channel_config_t ch_cfg = {
            .active_thresh = { (i == 0) ? TOUCH_THR_CH0 : TOUCH_THR_BAR },
            .charge_speed = TOUCH_CHARGE_SPEED_7,
            .init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT,
        };
        ESP_ERROR_CHECK(touch_sensor_new_channel(s_sens, s_ch_id[i], &ch_cfg, &s_chan[i]));
    }

    /* 硬件信号链: benchmark IIR + denoise 带 + smooth 滤波 + 激活滞回 + 去抖。
     * 硬件自身的比较结果 (mask) 已不参与判定, 只留作诊断对照 (见扫描循环 hw=)。
     * debounce_cnt 保持驱动默认 2: 实测它同时卡住释放 (手指离开 60ms 仍不报),
     * 所以释放改由软件判 (TOUCH_REL_DROP_*)。
     * smooth_filter 关掉 (驱动默认 IIR_2: smooth = 1/2·raw + 1/2·上次):
     * 逐帧实测它就是"快速双击被判成一次长按"的直接原因 —— 手指抬起时 raw 一步
     * 回到空载, 但 IIR 只能减半, 于是两次点击之间的谷底被抬到与"按住不动时的
     * d"同一区间 —— **谷底与按住两个区间重叠**, 任何绝对
     * 释放线都分不开它们。关掉后谷底直接回空载。
     * ⚠ 代价: 读数不再被平均, 空载峰升到阈值的 ~1/4 (CH0) 与 0.7~0.9 倍 (CH1)
     *   —— 滑条那侧余量本来就在临界线上 */
    touch_sensor_filter_config_t filt_cfg = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    filt_cfg.benchmark.filter_mode = TOUCH_BM_IIR_FILTER_16; /* 基线慢跟, 不被按压带走 */
    filt_cfg.data.smooth_filter = TOUCH_SMOOTH_NO_FILTER;
    ESP_ERROR_CHECK(touch_sensor_config_filter(s_sens, &filt_cfg));

    /* 回调只能在未使能时注册 */
    touch_event_callbacks_t cbs = {
        .on_active = touch_on_active,
        .on_inactive = touch_on_inactive,
    };
    ESP_ERROR_CHECK(touch_sensor_register_callbacks(s_sens, &cbs, NULL));

    ESP_ERROR_CHECK(touch_sensor_enable(s_sens));
    /* 前两次测量无效 (充电注入未稳), 与官方示例同样丢弃 3 次 */
    for (int k = 0; k < 3; k++)
        ESP_ERROR_CHECK(touch_sensor_trigger_oneshot_scanning(s_sens, 200));

    /* 基准复位 —— 这一步是「开机后某通道整段会话被锁成按住」的正解。
     * benchmark 由 IIR 从初值爬升, 三次 oneshot 保证的只是充电注入稳, 不是
     * benchmark 收敛: 同板多次开机里 benchmark 收敛并不一致, 有时停在远低于
     * raw 的值上 → smooth−benchmark 恒越阈 →
     * 该通道从开机起就被判成按住, 顶部滑条整条失效 (旧代码里表现为 ref 冻结的
     * 单向门)。do_reset 把 benchmark 直接置为当前测量值, 让每次开机的初值确定,
     * 不再是掷骰子。必须在 oneshot 预热之后(读数才有意义)、连续扫描之前。 */
    for (int i = 0; i < TOUCH_CH_COUNT; i++)
        ESP_ERROR_CHECK(touch_channel_config_benchmark(
            s_chan[i], &(touch_chan_benchmark_config_t){ .do_reset = true }));

    ESP_ERROR_CHECK(touch_sensor_start_continuous_scanning(s_sens));

    /* 必须在连续扫描跑起来之后再等一轮才回读 benchmark: do_reset 写的是
     * touch_channel_clr, benchmark 要等下一次测量才被装成 raw —— 复位后立刻读
     * 拿到的是 0x3FFFFF (22 位满量程)。实测踩过: 这个垃圾值被当成有效读数,
     * 整条判定链失效 (touch_log_hw_baseline 对它专门告警)。
     * 12 通道 × 1667us ≈ 20ms 一轮, 100ms 留 5 倍余量 */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* 回读确认阈值真的写进了驱动并缓存给 touch_fpc_get_thr — 运行期不再取
     * 驱动互斥锁 (get_channel_info 会拿 base->mutex) */
    for (int i = 0; i < TOUCH_CH_COUNT; i++) {
        touch_chan_info_t info = {0};
        if (touch_sensor_get_channel_info(s_chan[i], &info) == ESP_OK)
            s_thr_hw[i] = info.active_thresh[0];
    }
    ESP_LOGI(TAG, "硬件阈值: CH0=%u CH1..11=%u (回读自驱动)",
             (unsigned)s_thr_hw[0], (unsigned)s_thr_hw[1]);

    /* 同步软件基线 (息屏探针用) + 打印基准 (CH1 那类"基准没爬到位"全靠这行看) */
    touch_log_hw_baseline();

    /* 创建周期性扫描定时器 (亮屏固定 50Hz, 不再动态降档 — 见 SCAN_ON_MS 注释) */
    s_scan_timer = xTimerCreate(
        "touch_scan",
        pdMS_TO_TICKS(SCAN_ON_MS),
        pdTRUE, /* 自动重载 */
        NULL,
        touch_scan_timer_cb);
    if (!s_scan_timer) {
        ESP_LOGE(TAG, "创建扫描定时器失败");
        return ESP_ERR_NO_MEM;
    }
    s_last_activity = xTaskGetTickCount(); /* 启动即活动态 (50Hz 起步) */
    s_next_peak = s_last_activity + pdMS_TO_TICKS(TUNE_PEAK_PERIOD_MS); /* 首窗完整 */
    xTimerStart(s_scan_timer, 0);

    /* 独立探针任务 + esp_timer 节拍 (息屏期 20Hz/2Hz, 亮屏期阻塞零开销) */
    esp_timer_create_args_t targs = {
        .callback = probe_timer_cb,
        .arg = NULL,
        .name = "touch_probe",
    };
    if (esp_timer_create(&targs, &s_probe_timer) != ESP_OK)
        ESP_LOGE(TAG, "创建探针 esp_timer 失败");
    /* 互斥量必须先于任务创建 —— 任务一启动就会 take */
    s_probe_mtx = xSemaphoreCreateMutex();
    if (!s_probe_mtx) {
        ESP_LOGE(TAG, "创建探针互斥量失败");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(probe_task_fn, "touch_prb", 4096, NULL, 6, &s_probe_task)
        != pdPASS)
        ESP_LOGE(TAG, "创建探针任务失败");

    ESP_LOGI(TAG, "触摸 FPC 已初始化（12 通道 v2 驱动 + 硬件链, 亮屏恒 50Hz + 息屏 20/2Hz, 探针独立任务）");
    return ESP_OK;
}

/* ── 睡眠节电 ── */

void touch_fpc_pause(void)
{
    if (s_scan_timer) xTimerStop(s_scan_timer, 0);

    /* 停硬件连续扫描 (FSM 定时器真停, 不是只停软件读取) — 息屏期改由探针
     * 按档位触发 oneshot (见 touch_fpc_sleep_probe)。这是息屏功耗的主要来源:
     * 连续扫描 12 通道每 ~60ms 一巡且常开。
     * ⚠ 副作用: legacy 靠 FSM 定时器恒开把 RTC_PERIPH 隐式保电, 停扫后这层
     *   保障消失 → 靠 touch_fpc_init 里补的 esp_sleep_pd_config(RTC_PERIPH, ON)
     *   顶上。缺了它轻睡会清掉触摸寄存器/benchmark。
     * 失败只告警不 abort: pause 是关屏路径, 在这里 abort 等于关屏即死机。
     * 与探针互斥 (见 s_probe_mtx): 上一轮息屏的 oneshot 可能还没收尾。等待有界,
     * 同 touch_fpc_resume 的说明 */
    esp_err_t err = ESP_ERR_TIMEOUT;
    if (xSemaphoreTake(s_probe_mtx, pdMS_TO_TICKS(250)) == pdTRUE) {
        err = touch_sensor_stop_continuous_scanning(s_sens);
        xSemaphoreGive(s_probe_mtx);
    }
    if (err != ESP_OK)
        ESP_LOGW(TAG, "停硬件扫描失败 (%s) — 息屏功耗会偏高", esp_err_to_name(err));

    s_oneshot_failed = false; /* 新一轮息屏, 让 oneshot 告警重新可见 */
    s_probe_hits = 0;
    s_wake_pending = false;
    /* 息屏瞬态免疫窗 (1500ms): PA 关断/面板睡眠/电源整定引起的瞬态
     * ~900ms 内衰尽, 窗口留 2× 余量, 窗内不判唤醒。手动唤醒 (左键/
     * 摇动/硬件触摸退出回调) 不经过探针免疫窗, 息屏即摸不受影响 */
    s_immune_until = xTaskGetTickCount() + pdMS_TO_TICKS(1500);
    touch_note_activity(); /* 屏刚灭 = 快探档起步, 15s 无活动才降深闲 */

    /* 启动独立探针任务 (esp_timer 20Hz 节拍 — 见 probe_task_fn) */
    s_probe_enabled = true;
    if (s_probe_task) xTaskNotifyGive(s_probe_task);
}

void touch_fpc_resume(void)
{
    /* 亮屏判定是硬件的, 硬件 benchmark 自己跟环境漂移 → 这里不需要重置基准。
     * 反而不能重置: 用户多半正是"按着设备"把它唤醒的, do_reset 会把 benchmark
     * 钉在带手指的读数上, 松手后 smooth−benchmark 恒负 → 该通道装死, 且要等
     * IIR_16 慢慢爬回来。
     * 这里只把硬件 benchmark 抄给软件 ref (息屏探针的起点), 带手指污染守卫。
     * 先停扫描定时器防并发读写。 */
    if (s_scan_timer) xTimerStop(s_scan_timer, 0);
    s_probe_hits = 0;
    s_wake_pending = false;
    s_probe_enabled = false; /* 停独立探针任务 (任务内 ≤600ms 收尾停 esp_timer) */

    /* 恢复硬件连续扫描 (pause 停的)。**必须与探针互斥** (见 s_probe_mtx):
     * 息屏转亮屏的这一刻探针任务多半正卡在 oneshot 里 (is_started=true), 直接
     * start 会 ESP_ERR_INVALID_STATE —— 那个失败是永久的 (再也没有别的地方
     * start), 亮屏触摸全程失灵。
     * 等锁 250ms 而**不是** portMAX_DELAY: 等待量有界 (oneshot 十几 ms, 且驱动
     * 自己按 timeout 给整轮扫描兜了 200ms 硬上限, 见 touch_sens_common.c:385-404),
     * 超过就说明探针任务真卡死了 —— 那时告警比死等有用。本路径不在 LVGL 定时器
     * 上下文 (屏灭期间 lv_timers 已停), 不会被这里的等待卡住 UI。
     * 失败仍只告警不 abort —— 后者会变成"一唤醒就重启"死循环 */
    esp_err_t err = ESP_ERR_TIMEOUT;
    if (xSemaphoreTake(s_probe_mtx, pdMS_TO_TICKS(250)) == pdTRUE) {
        err = touch_sensor_start_continuous_scanning(s_sens);
        xSemaphoreGive(s_probe_mtx);
    }
    if (err != ESP_OK)
        ESP_LOGW(TAG, "恢复硬件扫描失败 (%s) — 亮屏触摸会失灵", esp_err_to_name(err));

    touch_log_hw_baseline();
    touch_note_activity();
    if (s_scan_timer) {
        xTimerChangePeriod(s_scan_timer, pdMS_TO_TICKS(SCAN_ON_MS), 0);
        xTimerStart(s_scan_timer, 0);
    }
}

/* ── 公开查询接口 ── */
bool touch_is_left_pressed(void) { return s_ts.touched[0]; }
float touch_top_position_raw(void) { return s_ts.raw_top_pos; }
float touch_right_position(void) { return s_ts.smooth_right_pos; }

bool touch_is_top_pressed(void)
{
    /* 顶部滑条 5 通道 (索引 1-5) 任一被触摸 */
    for (int i = 1; i <= TOUCH_TOP_CH_COUNT; i++) {
        if (s_ts.touched[i]) return true;
    }
    return false;
}

bool touch_is_right_pressed(void)
{
    /* Check if any right-side channel (6-11) is actually touched */
    for (int i = TOUCH_TOP_CH_COUNT + 1; i < TOUCH_CH_COUNT; i++) {
        if (s_ts.touched[i]) return true;
    }
    return false;
}

void touch_get_raw(uint32_t *out) { memcpy(out, s_ts.raw, sizeof(s_ts.raw)); }
void touch_get_baseline(uint32_t *out)
{
    /* 基线 = 硬件 benchmark。与 touch_get_raw 的 smooth 同源, 相减就是判定
     * 用的那个量 → power_log.csv 的 d 列与硬件 active_thresh 可直接对照 */
    for (int i = 0; i < TOUCH_CH_COUNT; i++)
        out[i] = s_ts.bm[i];
}
void touch_get_filtered(int *out) { memcpy(out, s_ts.filtered, sizeof(s_ts.filtered)); }
