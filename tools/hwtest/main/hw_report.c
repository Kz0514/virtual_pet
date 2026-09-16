/**
 * @file hw_report.c
 * @brief 测试项登记簿实现 (纯内存, 无并发 — 全部测试串行跑在主任务)
 */
#include "hw_report.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "hw_expect.h"

static const char *TAG = "hwtest";

static hw_item_t s_items[HW_MAX_ITEMS];
static int s_n = 0;

const char *hw_st_name(hw_st_t st)
{
    switch (st) {
    case HW_ST_PASS: return "PASS";
    case HW_ST_FAIL: return "FAIL";
    case HW_ST_SKIP: return "SKIP";
    default:         return "WARN";
    }
}

/* vsnprintf 在字节边界上硬切, 中文被切一半 → JSON/表格里出 �。从尾部回溯:
 * 最后那个字符的首字节若声明 n 字节而实际不足 n, 就把这个残缺字符砍掉。 */
static void utf8_trim(char *s)
{
    size_t n = strlen(s);
    size_t i = n;
    while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--; /* 跳过续字节 */
    if (i == 0) return;
    unsigned char c = (unsigned char)s[i - 1];
    size_t need = ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xE0) ? 3 : ((c & 0xF8) == 0xF0) ? 4 : 1;
    if (n - (i - 1) < need) s[i - 1] = '\0';
}

static void set_fmt(char *dst, size_t cap, const char *fmt, va_list ap)
{
    if (dst[0]) { /* 已有内容 → 追加, 便于多次 hw_set 累积 */
        size_t used = strlen(dst);
        if (used < cap - 2) {
            dst[used++] = ' ';
            vsnprintf(dst + used, cap - used, fmt, ap);
        }
        utf8_trim(dst);
        return;
    }
    vsnprintf(dst, cap, fmt, ap);
    utf8_trim(dst);
}

hw_item_t *hw_begin(const char *id, const char *name)
{
    if (s_n >= HW_MAX_ITEMS) {
        ESP_LOGE(TAG, "登记簿满 (%d) — 丢弃 %s", HW_MAX_ITEMS, id);
        return NULL;
    }
    hw_item_t *it = &s_items[s_n++];
    memset(it, 0, sizeof(*it));
    snprintf(it->id, sizeof(it->id), "%s", id);
    snprintf(it->name, sizeof(it->name), "%s", name);
    it->st = HW_ST_PASS; /* 默认通过; 失败由 hw_end 显式改 */
    return it;
}

void hw_set(hw_item_t *it, const char *fmt, ...)
{
    if (!it) return;
    va_list ap;
    va_start(ap, fmt);
    set_fmt(it->val, sizeof(it->val), fmt, ap);
    va_end(ap);
}

void hw_note(hw_item_t *it, const char *fmt, ...)
{
    if (!it) return;
    va_list ap;
    va_start(ap, fmt);
    set_fmt(it->note, sizeof(it->note), fmt, ap);
    va_end(ap);
}

void hw_end(hw_item_t *it, hw_st_t st)
{
    if (!it) return;
    it->st = st;
    ESP_LOGI(TAG, "[%s] %-11s %s | %s%s%s", hw_st_name(st), it->id, it->name, it->val,
             it->note[0] ? " | " : "", it->note);
}

/* ── 一行写法 ── */
static void one(hw_st_t st, const char *id, const char *name, const char *fmt, va_list ap)
{
    hw_item_t *it = hw_begin(id, name);
    if (!it) return;
    if (fmt) {
        va_list ap2;
        va_copy(ap2, ap);
        set_fmt(it->val, sizeof(it->val), fmt, ap2);
        va_end(ap2);
    }
    hw_end(it, st);
}

void hw_ok(const char *id, const char *name, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); one(HW_ST_PASS, id, name, fmt, ap); va_end(ap);
}
void hw_bad(const char *id, const char *name, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); one(HW_ST_FAIL, id, name, fmt, ap); va_end(ap);
}
void hw_skip(const char *id, const char *name, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); one(HW_ST_SKIP, id, name, fmt, ap); va_end(ap);
}
void hw_warn(const char *id, const char *name, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); one(HW_ST_WARN, id, name, fmt, ap); va_end(ap);
}

int hw_total(void) { return s_n; }
const hw_item_t *hw_at(int idx) { return (idx >= 0 && idx < s_n) ? &s_items[idx] : NULL; }

int hw_count(hw_st_t st)
{
    int n = 0;
    for (int i = 0; i < s_n; i++)
        if (s_items[i].st == st) n++;
    return n;
}

/* ── JSON: id 限定 ASCII, 但 v/note 里可能混进 SSID 等外来串 → 必须转义 ── */
static void json_esc(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 2 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c < 0x20) {
            dst[o++] = ' '; /* 控制字符 (SSID 里可能有) 直接吃掉 */
        } else {
            dst[o++] = (char)c; /* UTF-8 多字节原样透传 */
        }
    }
    dst[o] = '\0';
}

void hw_json(char *out, size_t out_len)
{
    size_t used = 0;
    int w = snprintf(out, out_len, "{\"fw\":\"%s\",\"items\":[", HW_FW_VERSION);
    if (w < 0) return;
    used = (size_t)w;
    char ev[176], en[176]; /* 80B 源串转义后最坏翻倍 */
    int dropped = 0;
    for (int i = 0; i < s_n && used < out_len; i++) {
        const hw_item_t *it = &s_items[i];
        json_esc(ev, sizeof(ev), it->val);
        json_esc(en, sizeof(en), it->note);
        w = snprintf(out + used, out_len - used,
                     "%s{\"i\":%d,\"id\":\"%s\",\"name\":\"%s\",\"st\":\"%s\",\"v\":\"%s\","
                     "\"note\":\"%s\"}",
                     i ? "," : "", i, it->id, it->name, hw_st_name(it->st), ev, en);
        if (w < 0 || (size_t)w >= out_len - used) {
            /* 装不下: snprintf 已截断写入 → 砍掉这一项, 记 dropped
             * (run.py 见到 trunc 会报错, 不会把少项当成"全绿") */
            out[used] = '\0';
            dropped = s_n - i;
            break;
        }
        used += (size_t)w;
    }
    if (used < out_len) {
        snprintf(out + used, out_len - used,
                 "],\"pass\":%d,\"fail\":%d,\"skip\":%d,\"warn\":%d,\"n\":%d,\"trunc\":%d}",
                 hw_count(HW_ST_PASS), hw_count(HW_ST_FAIL), hw_count(HW_ST_SKIP),
                 hw_count(HW_ST_WARN), s_n, dropped);
    }
}

void hw_dump_serial(void)
{
    ESP_LOGI(TAG, "══════ 判定汇总: PASS %d / FAIL %d / WARN %d / SKIP %d (共 %d) ══════",
             hw_count(HW_ST_PASS), hw_count(HW_ST_FAIL), hw_count(HW_ST_WARN),
             hw_count(HW_ST_SKIP), s_n);
    for (int i = 0; i < s_n; i++) {
        const hw_item_t *it = &s_items[i];
        if (it->st == HW_ST_PASS) continue;
        ESP_LOGE(TAG, "  ✘ #%02d [%s] %s %s | %s %s", i, hw_st_name(it->st), it->id, it->name,
                 it->val, it->note);
    }
    /* 静态: 56 项 × ~290B (含中文名+值+备注) 最坏 ~16KB;
     * 放栈上会爆主任务栈 (8192), 所以用 .bss */
    static char json[24576];
    hw_json(json, sizeof(json));
    /* 单行定界: run.py 只认这一行 */
    printf("HWTEST %s\n", json);
    fflush(stdout);
}
