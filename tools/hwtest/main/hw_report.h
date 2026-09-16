/**
 * @file hw_report.h
 * @brief 测试项登记簿 —— 所有模块只往这里写结论; main.c 统一输出串口 JSON + 屏幕看板。
 *
 * 判定语义:
 *   PASS 实测与期望一致
 *   FAIL 不一致 / 器件不应答 / 初始化报错
 *   SKIP 条件不满足 (无电池, 未烧 assets, 被命令行开关关掉) — 不算错
 *   WARN 器件活着但数值不符预期 (需人看一眼, 不判死)
 */
#ifndef HW_REPORT_H
#define HW_REPORT_H

#include <stdbool.h>
#include <stddef.h>

#define HW_MAX_ITEMS 56 /* 屏幕看板 8×7 格, 与之一致 */

typedef enum {
    HW_ST_PASS = 0,
    HW_ST_FAIL,
    HW_ST_SKIP,
    HW_ST_WARN,
} hw_st_t;

typedef struct {
    char id[20];   /* 机器可读键, 全 ASCII 大写/数字/'.'/'_' (屏幕字模只认这些) */
    char name[40]; /* 中文短名 (串口/日志用) */
    hw_st_t st;
    char val[128];  /* 实测值摘要 (够放"应答者清单 + 读数"这类长行) */
    char note[128]; /* 失败原因 / 备注 */
} hw_item_t;

/* 开始一项: 返回句柄 (登记簿定长数组, 指针稳定); 项数超上限时返回 NULL 并忽略后续写入 */
hw_item_t *hw_begin(const char *id, const char *name);
void hw_set(hw_item_t *it, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void hw_note(hw_item_t *it, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void hw_end(hw_item_t *it, hw_st_t st);

/* 一行写法 (够用时不拿句柄) */
void hw_ok(const char *id, const char *name, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void hw_bad(const char *id, const char *name, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void hw_skip(const char *id, const char *name, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void hw_warn(const char *id, const char *name, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

int hw_total(void);
int hw_count(hw_st_t st);
const hw_item_t *hw_at(int idx);
const char *hw_st_name(hw_st_t st);

/* 串口逐项一行 + 末尾 HWTEST {json} 一行 */
void hw_dump_serial(void);
/* JSON 摘要 (给 run.py 解析) */
void hw_json(char *out, size_t out_len);

#endif /* HW_REPORT_H */
