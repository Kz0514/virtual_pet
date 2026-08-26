/**
 * @file diary_screen.c
 * @brief 日记浏览 — 列表 + 信纸风详情
 *
 * 数据: /data/diary/YYYY-MM-DD.html (diary_sync 从服务端同步的单文件信纸页,
 * 文字 + 涂鸦 base64 内嵌)。
 * 列表: 扫描目录按日期倒序, 只显示日期+星期 (不读文件内容 — 最大 30 篇,
 * 逐篇解析太重); 5 行窗口可滚动, CONFIRM 进详情。
 * 详情: 读文件 → 按服务端模板提取 标题/心情/正文/涂鸦b64 → 反转义 HTML 实体
 * → 涂鸦 WebP 解码 (libwebp API 直解, 不经 LVGL FS) → 信纸风渲染,
 * UP/DOWN 滚动 (内容可能超一屏), BACK 回列表。
 * 涂鸦内存: 服务端已缩至 ≤240×240 透明 WebP; 解码产物 BGRA 像素
 * ≤230KB 全在 PSRAM (heap_caps SPIRAM), 退出详情释放 — 不常驻。
 * 交互: 事件经 input_handler 路由 (diary_screen_is_active 优先于设置页),
 * 输入枚举复用 menu_event_t (UP/DOWN/CONFIRM/BACK 语义一致)。
 */
#include "diary_screen.h"
#include "screen_switch.h"
#include "font_loader.h"
#include "tm6604.h"
#include "config_mgr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "webp/decode.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

static const char *TAG = "diary";

#define DIARY_DIR "/data/diary"
#define MAX_ENTRIES 32        /* 服务端配额 30 篇 + 余量 */
#define MAX_VISIBLE 5         /* 列表可视行数 */
#define ITEM_H 42             /* 28 顶栏 + 5×42 = 238 ≤ 240 */
#define HTML_CAP (192 * 1024) /* 单篇 HTML 上限 (diary_sync 同款) */
#define DOODLE_MAX_SIDE 240   /* 服务端已缩, 解码前防呆校验 */
#define VIBE_THROTTLE_MS 100

/* 滑动反向开关 (NVS "scroll_flip", 设置页"操作"页可调):
 * 开 → 详情 UP/DOWN 翻页方向反转 */
#define CFG_KEY_SCROLL_FLIP "scroll_flip"

/* ── 列表条目 ── */
typedef struct {
    char date[16]; /* YYYY-MM-DD */
} diary_entry_t;

static diary_entry_t s_entries[MAX_ENTRIES];
static int s_count = 0;
static int s_sel = 0;
static int s_win = 0; /* 列表窗口起点 (s_sel ∈ [s_win, s_win+5)) */

/* ── 详情数据 (打开详情时持有, 退出释放) ── */
static char *d_title = NULL; /* PSRAM */
static char *d_mood = NULL;  /* PSRAM, 可为空 */
static char *d_text = NULL;  /* PSRAM 正文 (段间 \n) */
static uint8_t *d_px = NULL; /* PSRAM 解码后涂鸦像素 (BGRA) */
static int d_px_w = 0, d_px_h = 0;
static lv_image_dsc_t s_doodle_dsc;

/* ── UI ── */
static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_prev_scr = NULL; /* 打开日记前的 screen (设置页) */
static lv_obj_t *s_title = NULL;
static lv_obj_t *s_rows[MAX_VISIBLE];
static lv_obj_t *s_row_lbl[MAX_VISIBLE];
static lv_obj_t *s_empty = NULL; /* 空列表提示 */
static lv_obj_t *s_cont = NULL;  /* 详情滚动容器 */
static bool s_active = false;
static bool s_detail = false;
static uint32_t s_last_vib = 0;

static const char *s_wday_cn[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};

/* 导航震动 (与设置页同款, 100ms 节流) */
static void nav_vibe(void)
{
    uint32_t now = xTaskGetTickCount();
    if (now - s_last_vib >= pdMS_TO_TICKS(VIBE_THROTTLE_MS)) {
        s_last_vib = now;
        tm6604_vibrate_raw(537, 40);
    }
}

/* 日期 → 星期 (0=周日); 非法返回 -1 */
static int date_wday(const char *ymd)
{
    int y, m, d;
    if (sscanf(ymd, "%d-%d-%d", &y, &m, &d) != 3) return -1;
    struct tm tm = {0};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    if (mktime(&tm) == (time_t)-1) return -1;
    return tm.tm_wday;
}

/* ── 列表扫描: /data/diary/YYYY-MM-DD.html → 日期倒序 ── */
static void scan_entries(void)
{
    s_count = 0;
    DIR *d = opendir(DIARY_DIR);
    if (!d) return; /* 目录不存在 = 无日记 */
    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_count < MAX_ENTRIES) {
        const char *n = e->d_name;
        if (strlen(n) != 15 || strcmp(n + 10, ".html") != 0) continue;
        char date[16];
        memcpy(date, n, 10);
        date[10] = '\0';
        if (date_wday(date) < 0) continue; /* 日期格式校验 */
        /* 插入排序: 日期字典序 = 时间序, 倒序 (新在前) */
        int i = s_count - 1;
        while (i >= 0 && strcmp(s_entries[i].date, date) < 0) {
            s_entries[i + 1] = s_entries[i];
            i--;
        }
        memcpy(s_entries[i + 1].date, date, sizeof(date));
        s_count++;
    }
    closedir(d);
    ESP_LOGI(TAG, "扫描到 %d 篇日记", s_count);
}

/* ── 读文件 (PSRAM 缓冲, ≤HTML_CAP) ── */
static char *read_file(const char *path, size_t *out_len)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > HTML_CAP) {
        return NULL;
    }
    char *buf = heap_caps_malloc((size_t)st.st_size + 1, MALLOC_CAP_SPIRAM);
    if (!buf) return NULL;
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        heap_caps_free(buf);
        return NULL;
    }
    ssize_t r = read(fd, buf, (size_t)st.st_size);
    close(fd);
    if (r < 0) r = 0;
    buf[r] = '\0';
    if (out_len) *out_len = (size_t)r;
    return buf;
}

/* ── HTML 实体反转义 (就地) — 服务端 html.escape 的五种 ── */
static void html_unescape(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (strncmp(r, "&amp;", 5) == 0) {
            *w++ = '&';
            r += 5;
        } else if (strncmp(r, "&lt;", 4) == 0) {
            *w++ = '<';
            r += 4;
        } else if (strncmp(r, "&gt;", 4) == 0) {
            *w++ = '>';
            r += 4;
        } else if (strncmp(r, "&quot;", 6) == 0) {
            *w++ = '"';
            r += 6;
        } else if (strncmp(r, "&#x27;", 6) == 0 ||
                   strncmp(r, "&#39;", 5) == 0) {
            *w++ = '\'';
            r += (r[3] == 'x') ? 6 : 5;
        } else
            *w++ = *r++;
    }
    *w = '\0';
}

/* 提取 <open>…</close> 内的文本 (拷贝到 PSRAM + 反转义); 缺省返回 NULL */
static char *tag_body_copy(const char *html, const char *open, const char *close)
{
    const char *s = strstr(html, open);
    if (!s) return NULL;
    s += strlen(open);
    const char *e = strstr(s, close);
    if (!e) return NULL;
    size_t len = (size_t)(e - s);
    char *out = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (!out) return NULL;
    memcpy(out, s, len);
    out[len] = '\0';
    html_unescape(out);
    return out;
}

/* 正文: <div class="content">…</div> 内去 <p>/</p> 标签
 * 段间 \n\n 分隔 (build_detail_ui 按 \n\n 拆独立 label, 段距由容器 pad_row
 * 独立控制, 不再与行距耦合) + 每段首行缩进两个全角空格 (信纸风, 与服务端
 * text-indent: 2em 一致)。容量: 每段输出 ≤ 输入+8, cap 放大 3 倍保险。 */
static char *parse_content(const char *html)
{
    const char *s = strstr(html, "<div class=\"content\">");
    if (!s) return NULL;
    s += strlen("<div class=\"content\">");
    const char *e = strstr(s, "</div>");
    if (!e) return NULL;
    size_t cap = (size_t)(e - s) * 3 + 8;
    char *out = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!out) return NULL;
    size_t o = 0;
    const char *p = s;
    bool first = true;
    while (p < e && o + 8 < cap) {
        if (strncmp(p, "<p>", 3) == 0) {
            /* 首段不换行, 段间空一行 — -6 负行距下空行段距 32px, 紧凑且段落可辨 */
            if (!first) {
                out[o++] = '\n';
                out[o++] = '\n';
            }
            first = false;
            /* 首行缩进: 两个全角空格 U+3000 */
            out[o++] = '\xE3';
            out[o++] = '\x80';
            out[o++] = '\x80';
            out[o++] = '\xE3';
            out[o++] = '\x80';
            out[o++] = '\x80';
            p += 3;
        } else if (strncmp(p, "</p>", 4) == 0) {
            p += 4;
        } else
            out[o++] = *p++;
    }
    out[o] = '\0';
    html_unescape(out);
    return out;
}

/* ── base64 解码 (标准字母表 + padding; 服务端 b64encode 无换行) ── */
static int b64_char(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t b64_decode(const char *in, size_t inlen, uint8_t *out)
{
    size_t o = 0;
    int acc = 0, bits = 0;
    for (size_t i = 0; i < inlen; i++) {
        if (in[i] == '=') break;
        int v = b64_char(in[i]);
        if (v < 0) continue;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out) out[o] = (uint8_t)((acc >> bits) & 0xFF);
            o++;
        }
    }
    return o;
}

/* ── 涂鸦: b64 → WebP 字节 → BGRA 像素 (PSRAM, ≤230KB) ──
 * LVGL ARGB8888 内存序为 B,G,R,A (lv_color32_t) → WebPDecodeBGRAInto,
 * 与 lv_libwebp.c 的 MODE_BGRA 一致。 */
static bool decode_doodle(const char *b64, size_t b64len)
{
    if (d_px) return true; /* 已解码 */
    size_t raw_len = b64_decode(b64, b64len, NULL);
    if (raw_len < 20) return false;
    uint8_t *raw = heap_caps_malloc(raw_len + 4, MALLOC_CAP_SPIRAM);
    if (!raw) return false;
    b64_decode(b64, b64len, raw);

    int w, h;
    if (!WebPGetInfo(raw, raw_len, &w, &h) ||
        w <= 0 || w > DOODLE_MAX_SIDE || h <= 0 || h > DOODLE_MAX_SIDE) {
        heap_caps_free(raw);
        return false;
    }
    size_t px_len = (size_t)w * h * 4;
    uint8_t *px = heap_caps_malloc(px_len, MALLOC_CAP_SPIRAM);
    if (!px) {
        heap_caps_free(raw);
        return false;
    }
    if (WebPDecodeBGRAInto(raw, raw_len, px, px_len, w * 4) == NULL) {
        heap_caps_free(raw);
        heap_caps_free(px);
        return false;
    }
    heap_caps_free(raw);

    d_px = px;
    d_px_w = w;
    d_px_h = h;
    ESP_LOGI(TAG, "涂鸦解码 %dx%d (%u B)", w, h, (unsigned)px_len);
    return true;
}

/* ── 详情数据释放 ── */
static void detail_free(void)
{
    if (d_title) {
        heap_caps_free(d_title);
        d_title = NULL;
    }
    if (d_mood) {
        heap_caps_free(d_mood);
        d_mood = NULL;
    }
    if (d_text) {
        heap_caps_free(d_text);
        d_text = NULL;
    }
    if (d_px) {
        heap_caps_free(d_px);
        d_px = NULL;
    }
    d_px_w = d_px_h = 0;
    memset(&s_doodle_dsc, 0, sizeof(s_doodle_dsc));
}

/* ── 列表 UI ── */
static void build_list_ui(void)
{
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    /* 顶栏 */
    lv_obj_t *hdr = lv_obj_create(s_scr);
    lv_obj_set_size(hdr, 240, 28);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icons = lv_label_create(hdr);
    lv_label_set_text(icons, LV_SYMBOL_LEFT " " LV_SYMBOL_LIST);
    lv_obj_set_style_text_color(icons, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(icons, &lv_font_montserrat_14, 0);
    lv_obj_align(icons, LV_ALIGN_LEFT_MID, 10, 0);

    s_title = lv_label_create(hdr);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xcccccc), 0);
    if (FONT_ZH)
        lv_obj_set_style_text_font(s_title, FONT_ZH, 0);
    else
        lv_obj_set_style_text_font(s_title, &lv_font_montserrat_14, 0);
    lv_obj_align(s_title, LV_ALIGN_LEFT_MID, 48, 0);

    /* 空列表提示 (默认隐藏) */
    s_empty = lv_label_create(s_scr);
    char empty_txt[64];
    snprintf(empty_txt, sizeof(empty_txt), "还没有日记…\n多和%s互动吧",
             config_get_str("pet_name", "萝莉丝"));
    lv_label_set_text(s_empty, empty_txt);
    lv_obj_set_style_text_color(s_empty, lv_color_hex(0x555555), 0);
    if (FONT_ZH) lv_obj_set_style_text_font(s_empty, FONT_ZH, 0);
    lv_obj_set_style_text_align(s_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_empty);
    lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);

    /* 5 行 */
    for (int i = 0; i < MAX_VISIBLE; i++) {
        lv_obj_t *row = lv_obj_create(s_scr);
        lv_obj_set_size(row, 240, ITEM_H);
        lv_obj_set_pos(row, 0, 30 + i * ITEM_H);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(row);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xeeeeee), 0);
        if (FONT_ZH)
            lv_obj_set_style_text_font(lbl, FONT_ZH, 0);
        else
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 10, 0);

        s_rows[i] = row;
        s_row_lbl[i] = lbl;
    }
}

static void refresh_list(void)
{
    lv_label_set_text(s_title, "日记");

    if (s_count == 0) {
        lv_obj_clear_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < MAX_VISIBLE; i++) {
            lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < MAX_VISIBLE; i++) {
        int idx = s_win + i;
        if (idx >= s_count) {
            lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);

        bool sel = (idx == s_sel);
        lv_obj_set_style_bg_color(s_rows[i],
                                  sel ? lv_color_hex(0x333344) : lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_rows[i],
                                sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);

        char line[32];
        int wd = date_wday(s_entries[idx].date);
        snprintf(line, sizeof(line), "%s %s",
                 s_entries[idx].date, wd >= 0 ? s_wday_cn[wd] : "");
        lv_label_set_text(s_row_lbl[i], line);
        lv_obj_set_style_text_color(s_row_lbl[i],
                                    sel ? lv_color_hex(0xffffff) : lv_color_hex(0xbbbbbb), 0);
    }
}

/* ── 详情 UI (信纸风) ── */
static void build_detail_ui(const char *date)
{
    s_detail = true;
    lv_obj_clean(s_scr);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0xf3ead8), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    /* 顶栏: ← 日期 */
    lv_obj_t *hdr = lv_obj_create(s_scr);
    lv_obj_set_size(hdr, 240, 28);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0xe9dcc0), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icons = lv_label_create(hdr);
    lv_label_set_text(icons, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(icons, lv_color_hex(0x8a7355), 0);
    lv_obj_set_style_text_font(icons, &lv_font_montserrat_14, 0);
    lv_obj_align(icons, LV_ALIGN_LEFT_MID, 10, 0);

    char dt[24];
    int wd = date_wday(date);
    snprintf(dt, sizeof(dt), "%s %s", date, wd >= 0 ? s_wday_cn[wd] : "");
    lv_obj_t *t = lv_label_create(hdr);
    lv_label_set_text(t, dt);
    lv_obj_set_style_text_color(t, lv_color_hex(0x8a7355), 0);
    if (FONT_ZH)
        lv_obj_set_style_text_font(t, FONT_ZH, 0);
    else
        lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
    lv_obj_center(t);

    /* 滚动容器 */
    s_cont = lv_obj_create(s_scr);
    lv_obj_set_size(s_cont, 240, 212);
    lv_obj_set_pos(s_cont, 0, 28);
    lv_obj_set_scroll_dir(s_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(s_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_cont, 0, 0);
    lv_obj_set_style_radius(s_cont, 0, 0);
    lv_obj_set_style_pad_all(s_cont, 10, 0);
    lv_obj_set_style_pad_row(s_cont, 8, 0);
    lv_obj_set_flex_flow(s_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_flex_cross_place(s_cont, LV_FLEX_ALIGN_CENTER, 0);

    /* 标题组: 标题 + 信纸装饰线一体 (线贴标题下方, 不独占一行);
     * tbox 显式宽 216, 保持内容居中 */
    if (d_title && d_title[0]) {
        lv_obj_t *tbox = lv_obj_create(s_cont);
        lv_obj_set_size(tbox, 216, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(tbox, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(tbox, 0, 0);
        lv_obj_set_style_radius(tbox, 0, 0);
        lv_obj_set_style_pad_all(tbox, 0, 0);
        lv_obj_set_style_pad_row(tbox, 2, 0);
        lv_obj_clear_flag(tbox, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(tbox, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_flex_cross_place(tbox, LV_FLEX_ALIGN_CENTER, 0);

        lv_obj_t *h1 = lv_label_create(tbox);
        lv_label_set_text(h1, d_title);
        lv_label_set_long_mode(h1, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(h1, 216);
        lv_obj_set_style_text_color(h1, lv_color_hex(0x2a211a), 0);
        lv_obj_set_style_text_align(h1, LV_TEXT_ALIGN_CENTER, 0);
        if (FONT_ZH) lv_obj_set_style_text_font(h1, FONT_ZH, 0);
        /* 269 = LV_SCALE_NONE(256)×1.05 放大; pivot 50% 令放大以中心对称, 防偏移 */
        lv_obj_set_style_transform_scale(h1, 269, 0);
        lv_obj_set_style_transform_pivot_x(h1, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(h1, LV_PCT(50), 0);

        lv_obj_t *rule = lv_obj_create(tbox);
        lv_obj_set_size(rule, 52, 2);
        lv_obj_set_style_bg_color(rule, lv_color_hex(0xd8c9ad), 0);
        lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(rule, 0, 0);
        lv_obj_set_style_radius(rule, 1, 0);
        lv_obj_clear_flag(rule, LV_OBJ_FLAG_SCROLLABLE);
    }

    /* 心情 — 纯文字居中红字 */
    if (d_mood && d_mood[0]) {
        lv_obj_t *ml = lv_label_create(s_cont);
        lv_label_set_text(ml, d_mood);
        lv_obj_set_style_text_color(ml, lv_color_hex(0xd96459), 0);
        lv_obj_set_style_text_align(ml, LV_TEXT_ALIGN_CENTER, 0);
        if (FONT_ZH) lv_obj_set_style_text_font(ml, FONT_ZH, 0);
    }

    /* 正文: 段落拆独立 label — 段距由容器 pad_row 控制, 行距 line_space -6,
     * 互不耦合 */
    if (d_text && d_text[0]) {
        lv_obj_t *pbox = lv_obj_create(s_cont);
        lv_obj_set_size(pbox, 216, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(pbox, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(pbox, 0, 0);
        lv_obj_set_style_radius(pbox, 0, 0);
        lv_obj_set_style_pad_all(pbox, 0, 0);
        lv_obj_set_style_pad_row(pbox, 6, 0); /* 段落间距 6px */
        lv_obj_clear_flag(pbox, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(pbox, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_flex_cross_place(pbox, LV_FLEX_ALIGN_CENTER, 0);

        const char *p = d_text;
        while (p && *p) {
            const char *q = strstr(p, "\n\n");
            size_t plen = q ? (size_t)(q - p) : strlen(p);
            if (plen > 0) {
                char *para = heap_caps_malloc(plen + 1, MALLOC_CAP_SPIRAM);
                if (para) {
                    memcpy(para, p, plen);
                    para[plen] = '\0';
                    lv_obj_t *pl = lv_label_create(pbox);
                    lv_label_set_text(pl, para);
                    lv_label_set_long_mode(pl, LV_LABEL_LONG_WRAP);
                    lv_obj_set_width(pl, 216);
                    lv_obj_set_style_text_color(pl, lv_color_hex(0x4a3f35), 0);
                    /* zh.bin 行高 22px (16px 字) — 用负行距 (-6) 收紧段落行距 */
                    lv_obj_set_style_text_line_space(pl, -6, 0);
                    if (FONT_ZH) lv_obj_set_style_text_font(pl, FONT_ZH, 0);
                    heap_caps_free(para);
                }
            }
            if (!q) break;
            p = q + 2;
        }
    }

    /* 涂鸦 (透明 WebP 直接铺在信纸上) */
    if (d_px) {
        memset(&s_doodle_dsc, 0, sizeof(s_doodle_dsc));
        s_doodle_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_doodle_dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
        s_doodle_dsc.header.w = (uint16_t)d_px_w;
        s_doodle_dsc.header.h = (uint16_t)d_px_h;
        s_doodle_dsc.header.stride = (uint16_t)(d_px_w * 4);
        s_doodle_dsc.data_size = (uint32_t)((size_t)d_px_w * d_px_h * 4);
        s_doodle_dsc.data = d_px;
        lv_obj_t *img = lv_image_create(s_cont);
        lv_image_set_src(img, &s_doodle_dsc);
    }

    /* 页脚 (ASCII 连字符 — 中文字体子集缺 "·" 会显示 tofu) */
    lv_obj_t *foot = lv_label_create(s_cont);
    char foot_txt[64];
    snprintf(foot_txt, sizeof(foot_txt), "- %s的日记 -",
             config_get_str("pet_name", "萝莉丝"));
    lv_label_set_text(foot, foot_txt);
    lv_obj_set_style_text_color(foot, lv_color_hex(0xb3a28c), 0);
    if (FONT_ZH) lv_obj_set_style_text_font(foot, FONT_ZH, 0);

    lv_obj_update_layout(s_scr);
    uint32_t ci = 0;
    lv_obj_t *ch = NULL;
    while ((ch = lv_obj_get_child(s_cont, ci)) != NULL) {
        ESP_LOGI(TAG, "子元素%lu: pos(%d,%d) %dx%d", (unsigned long)ci,
                 lv_obj_get_x(ch), lv_obj_get_y(ch),
                 lv_obj_get_width(ch), lv_obj_get_height(ch));
        ci++;
    }
}

/* ── 详情: 读文件 + 解析 + 解码 ── */
static bool open_detail(int idx)
{
    detail_free();
    if (idx < 0 || idx >= s_count) return false;

    char path[64];
    snprintf(path, sizeof(path), "%s/%s.html", DIARY_DIR, s_entries[idx].date);
    size_t len = 0;
    char *html = read_file(path, &len);
    if (!html) {
        ESP_LOGW(TAG, "读取失败: %s", path);
        return false;
    }

    d_title = tag_body_copy(html, "<h1>", "</h1>");
    d_mood = tag_body_copy(html, "<div class=\"mood\">", "</div>");
    d_text = parse_content(html);

    const char *b64p = strstr(html, "data:image/webp;base64,");
    if (b64p) {
        b64p += strlen("data:image/webp;base64,");
        const char *q = strchr(b64p, '"');
        if (q)
            decode_doodle(b64p, (size_t)(q - b64p));
        else
            ESP_LOGW(TAG, "涂鸦 b64 截断");
    }
    heap_caps_free(html);

    if (!d_title && !d_mood && !d_text) {
        ESP_LOGW(TAG, "解析为空: %s", path);
        detail_free();
        return false;
    }
    build_detail_ui(s_entries[idx].date);
    return true;
}

/* ── 列表选择 ── */
static void move_sel(int dir)
{
    int next = s_sel + dir;
    if (next < 0 || next >= s_count) return;
    s_sel = next;
    if (s_sel < s_win) s_win = s_sel;
    if (s_sel >= s_win + MAX_VISIBLE) s_win = s_sel - MAX_VISIBLE + 1;
    refresh_list();
    nav_vibe();
}

/* ══════ 对外接口 ══════ */

void diary_screen_input(menu_event_t ev)
{
    if (!s_active) return;

    if (s_detail) {
        switch (ev) {
        case MENU_EV_UP:
        case MENU_EV_DOWN: {
            /* scroll_to_y 会 clamp 到可滚动范围 (scroll_by 会无限滚出边界) */
            int dy = (ev == MENU_EV_UP) ? -20 : 20;
            /* 滑动反向开关 (设置页"操作"页) — 翻页方向反转 */
            if (config_get_u32(CFG_KEY_SCROLL_FLIP, 0)) dy = -dy;
            lv_obj_scroll_to_y(s_cont, lv_obj_get_scroll_y(s_cont) + dy,
                               LV_ANIM_OFF);
            break;
        }
        case MENU_EV_BACK: {
            /* 删屏重建: 详情/列表切换走完整屏幕切换 (与 destroy 同机制),
             * 绕开部分刷新下 clean+重建的残留 */
            detail_free();
            s_detail = false;
            s_cont = NULL;
            lv_obj_t *old = s_scr;
            s_scr = lv_obj_create(NULL);
            lv_obj_set_scrollbar_mode(s_scr, LV_SCROLLBAR_MODE_OFF);
            build_list_ui();
            refresh_list();
            screen_load_full(s_scr);
            lv_obj_del(old);
            nav_vibe();
            break;
        }
        default:
            break;
        }
        return;
    }

    switch (ev) {
    case MENU_EV_UP:
        move_sel(-1);
        break;
    case MENU_EV_DOWN:
        move_sel(+1);
        break;
    case MENU_EV_CONFIRM:
        if (open_detail(s_sel)) nav_vibe();
        break;
    case MENU_EV_BACK:
        diary_screen_destroy();
        break;
    default:
        break;
    }
}

esp_err_t diary_screen_init(void)
{
    if (s_active) return ESP_OK;
    ESP_LOGI(TAG, "打开日记…");

    s_prev_scr = lv_screen_active();
    s_sel = 0;
    s_win = 0;
    s_detail = false;
    detail_free();
    scan_entries();

    s_scr = lv_obj_create(NULL);
    lv_obj_set_scrollbar_mode(s_scr, LV_SCROLLBAR_MODE_OFF);
    build_list_ui();
    refresh_list();

    s_active = true;
    screen_load_full(s_scr);
    ESP_LOGI(TAG, "日记列表已显示");
    return ESP_OK;
}

void diary_screen_destroy(void)
{
    if (!s_active) return;
    ESP_LOGI(TAG, "关闭日记");
    if (s_prev_scr) {
        /* 部分刷新下切屏漏刷 → 强制目标屏整屏重绘, 防残影 */
        screen_load_full(s_prev_scr);
    }
    detail_free();
    s_detail = false;
    if (s_scr) {
        lv_obj_del(s_scr);
        s_scr = NULL;
    }
    s_title = NULL;
    s_cont = NULL;
    s_empty = NULL;
    for (int i = 0; i < MAX_VISIBLE; i++) {
        s_rows[i] = NULL;
        s_row_lbl[i] = NULL;
    }
    s_active = false;
}

bool diary_screen_is_active(void)
{
    return s_active;
}
