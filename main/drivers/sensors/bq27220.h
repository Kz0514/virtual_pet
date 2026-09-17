/**
 * @file bq27220.h
 * @brief BQ27220 电池电量计 (I2C 0x55, SBS协议)
 *
 * 读取: 电压(mV)/电流(mA)/电量(%)/剩余容量(mAh)/温度(°C)/健康度/循环次数
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t voltage_mv;
    int16_t  current_ma;      /* +充电, -放电 */
    uint16_t soc_pct;         /* 0-100% */
    uint16_t remain_mah;
    uint16_t full_mah;
    float    temp_c;
    uint16_t health_pct;      /* SOH 0-100% */
    uint16_t cycle_count;
} bq27220_data_t;

esp_err_t bq27220_init(void);
esp_err_t bq27220_read(bq27220_data_t *out);
esp_err_t bq27220_read_soc(uint16_t *soc_pct);

/** 芯片 SOC 与电压是否自洽 (失步检测)。
 *  两条经验判据: 电压 ≥3.9V 却报 ≤10% (低向失步, 实测过);
 *  电压 ≤3.5V 却报 ≥90% (高向失步)。电压 ≈0 (无电池) 时不参与判断。 */
bool bq27220_soc_plausible(uint16_t soc_pct, uint16_t mv);

/** 电压法 SOC 估计 (0~100) — 失步期兜底用。
 *  查的是静置 OCV 粗对照表; 充放电大电流会抬升/压低读数, 故不作精确依据。 */
uint16_t bq27220_soc_from_mv(uint16_t mv);

/** 扫描 SBS 寄存器 (0x00-0x3F), 打印所有有效读数到日志 */
void bq27220_debug_scan(void);

/** 把数据内存里的容量标尺改成实际电芯容量 (FCC + DesignCapacity = 800mAh)。
 *  回读守卫: 值已对就不写。开机调一次 — 芯片 DM 可能是 RAM 影子 (POR 即丢)。
 *  失败只告警, 不影响启动。副作用: 退出配置态会重算一次 RC, SOC 会跳。 */
void bq27220_apply_capacity_cfg(void);

/** TEMP: 只读观测 — 开机 dump 配置区供跨重启比对, 再起一个 20 分钟任务
 *  采 ΔRC vs ∫I·dt。用于判定 RC 是否随电流积分, 验完即删。 */
void bq27220_probe_watch(void);

#ifdef __cplusplus
}
#endif
