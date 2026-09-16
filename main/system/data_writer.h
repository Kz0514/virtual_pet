/**
 * @file data_writer.h
 * @brief /data (FatFS) 写入统一入口 — 攒批 + 闸门 + 擦除计数
 *
 * 存在理由: FatFS 每次扇区写 = 先擦 4KB 再写 (diskio_wl, 无 journal),
 * 而 /data 的 FAT12 全表只占 1 个扇区、根目录 1 个扇区 — 每 close 一次
 * 就要重写这两个热区。写入点散落在各模块时次数无法收敛 (power_log 每
 * 2s 一次 ≈ 8.6 万次擦除/天), 而"攒批"与"统一闸门"只有单入口能实施。
 *
 * 架构: 投递-落盘分离。调用方 (任意任务上下文) 只 memcpy + 返回;
 * data_writer_tick() (2s 节拍) 检查到点的文件并落盘 — 到点一次
 * open→写全部→close, 擦除次数按批计而非按行计。TTS 播放 / 低电量 /
 * 未挂载 / U盘模式期间不落盘, 数据留内存 (缓冲满丢最老的行), 条件恢复
 * 后补写。
 *
 * 不建专用任务: 内部 RAM 仅剩 ~10KB, 再要 4KB 栈 + 4KB 缓冲会压垮
 * power_diag 的 8KB 堆守卫 (锁 dump 会永久停写)。缓冲放 PSRAM (PSRAM
 * 缓冲直接 write 到 /data 已被 diary_sync 的 128KB HTML 走通), 落盘借用
 * 调用方任务 — 主线每 30s 阻塞一次 100-400ms, 远好于现状的每 2s 一次。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"

typedef enum {
    /* /data/power_log.csv — 追加, 48KB 截断重开 */
    DW_POWER_LOG = 0,
    DW_FILE_N
} dw_file_t;

esp_err_t data_writer_init(void);

/* 投递一段文本 (立即返回)。未初始化 / 单块超缓冲 → 丢弃并计数, 返回 false。
 * 不判 /data 挂载 — 投递期挂载可能尚未完成, 落盘时才判 */
bool data_writer_append(dw_file_t f, const char *buf, int len);

/* FILE* 型 dump 适配 (esp_pm_dump_locks / esp_timer_dump 硬依赖 FILE*):
 * 回调在 data_writer_tick() 里执行 (已在落盘闸门之内), 收到的 fp 已按序
 * 接在本文件攒批之后 — 既不破坏 CSV 时序, 也不额外开一次落盘窗口。
 * 单槽: 已有未处理的请求时返回 false (诊断可丢) */
typedef void (*dw_emit_fn)(FILE *fp, void *arg);
bool data_writer_append_stream(dw_file_t f, dw_emit_fn emit, void *arg);

/* 2s 节拍调用 — 落盘执行点 (到点/到量/有 dump 待写时动作)。
 * 闸门关、/data 不可用、TTS 播放中 → 本轮不落盘, 数据继续攒。
 * 但 5 分钟一条的"写入统计"不受闸门影响 (被挂起时最需要它), 并带出原因 */
void data_writer_tick(void);

/* 写盘闸门 (main.c 2s 块统一求值, 与 memory_store_writes_safe 同源) */
void data_writer_set_gate(bool allow);
bool data_writer_gate_ok(void);
