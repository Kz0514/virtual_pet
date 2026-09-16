/**
 * @file hw_screen.c
 * @brief 自绘 RGB565 看板 —— 零依赖 (不引 LVGL), 5x7 字模手写表
 *
 * 字模只做"仪表盘够用"的一套: 0-9 + A E F H I L P S T W + 空格 : . - #
 * 不认识的字符画成实心方块 (一眼看出串里有没编进字模的字符)。
 */
#include "hw_screen.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "hw_expect.h"
#include "hw_periph.h"
#include "hw_report.h"

static const char *TAG = "hwtest";

/* ══════════════════════════════════════════════════════════════════════
 * 5x7 字模: 每字符 5 列, 每列低 7 位 (bit0 = 最上一行)
 * ══════════════════════════════════════════════════════════════════════ */
typedef struct {
    char ch;
    uint8_t col[5];
} glyph_t;

static const glyph_t FONT[] = {
    {'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}}, {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}}, {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7F, 0x10}}, {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}}, {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}}, {'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
    {'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}}, {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
    {'F', {0x7F, 0x09, 0x09, 0x09, 0x01}}, {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}},
    {'I', {0x00, 0x41, 0x7F, 0x41, 0x00}}, {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
    {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}}, {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
    {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}}, {'W', {0x7F, 0x20, 0x18, 0x20, 0x7F}},
    {'K', {0x7F, 0x08, 0x14, 0x22, 0x41}}, {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
    {'N', {0x7F, 0x02, 0x0C, 0x10, 0x7F}}, {' ', {0, 0, 0, 0, 0}},
    {':', {0x00, 0x36, 0x36, 0x00, 0x00}}, {'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
    {'-', {0x08, 0x08, 0x08, 0x08, 0x08}}, {'#', {0x14, 0x7F, 0x14, 0x7F, 0x14}},
};
#define FONT_N (sizeof(FONT) / sizeof(FONT[0]))

/* ── 布局 ── */
#define SCR_W 240
#define SCR_H 240
#define GRID_TOP 30
#define CELL_W 30
#define CELL_H 27

static uint16_t *s_fb = NULL;

static inline void px(int x, int y, uint16_t c)
{
    if (x < 0 || y < 0 || x >= SCR_W || y >= SCR_H) return;
    s_fb[y * SCR_W + x] = c;
}

static void fill_rect(int x, int y, int w, int h, uint16_t c)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) px(x + i, y + j, c);
}

static void draw_char(int x, int y, char ch, int scale, uint16_t c)
{
    const uint8_t *col = NULL;
    static const uint8_t unknown[5] = {0x7F, 0x41, 0x41, 0x41, 0x7F}; /* 方框 = 字模缺失 */
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    for (size_t k = 0; k < FONT_N; k++) {
        if (FONT[k].ch == ch) {
            col = FONT[k].col;
            break;
        }
    }
    if (!col) col = unknown;
    for (int cx = 0; cx < 5; cx++) {
        for (int cy = 0; cy < 7; cy++) {
            if (!(col[cx] & (1 << cy))) continue;
            fill_rect(x + cx * scale, y + cy * scale, scale, scale, c);
        }
    }
}

/* 字宽 = 6*scale - scale (末列不留间距) */
static int text_w(const char *s, int scale)
{
    int n = (int)strlen(s);
    if (n <= 0) return 0;
    return n * 6 * scale - scale;
}

static void draw_text(int x, int y, const char *s, int scale, uint16_t c)
{
    for (const char *p = s; *p; p++) {
        draw_char(x, y, *p, scale, c);
        x += 6 * scale;
    }
}

static void draw_text_centered(int y, const char *s, int scale, uint16_t c)
{
    draw_text((SCR_W - text_w(s, scale)) / 2, y, s, scale, c);
}

static uint16_t st_color(hw_st_t st)
{
    switch (st) {
    case HW_ST_PASS: return HW_C_PASS;
    case HW_ST_FAIL: return HW_C_FAIL;
    case HW_ST_SKIP: return HW_C_SKIP;
    default:         return HW_C_WARN;
    }
}

int hw_screen_dashboard(void)
{
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    if (hw_lcd_get(&panel, &io) != ESP_OK) {
        hw_warn("screen.board", "屏幕看板", "面板没起来 → 无看板 (串口 JSON 仍有效)");
        return ESP_ERR_INVALID_STATE;
    }

    /* 帧缓冲: 优先内部 RAM (推送时不占 PSRAM 带宽), 拿不到退 PSRAM */
    size_t sz = SCR_W * SCR_H * 2;
    s_fb = heap_caps_malloc(sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    bool internal = (s_fb != NULL);
    if (!s_fb) s_fb = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    if (!s_fb) {
        hw_warn("screen.board", "屏幕看板", "115KB 帧缓冲分配失败 (内部 %uKB / PSRAM %uKB)",
                (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
        return ESP_ERR_NO_MEM;
    }

    /* 底 + 表头 */
    fill_rect(0, 0, SCR_W, SCR_H, HW_C_BG);
    draw_text(6, 2, "HWTEST-1.0", 1, HW_C_TEXT);
    char line[32];
    snprintf(line, sizeof(line), "P%d F%d S%d W%d", hw_count(HW_ST_PASS), hw_count(HW_ST_FAIL),
             hw_count(HW_ST_SKIP), hw_count(HW_ST_WARN));
    draw_text_centered(12, line, 2, HW_C_TEXT);

    /* 56 格: 编号 = 登记簿下标 */
    int n = hw_total();
    for (int i = 0; i < HW_MAX_ITEMS; i++) {
        int col = i % 8, row = i / 8;
        int x0 = col * CELL_W, y0 = GRID_TOP + row * CELL_H;
        if (i >= n) {
            fill_rect(x0 + 1, y0 + 1, CELL_W - 2, CELL_H - 2, 0x2104); /* 空位 */
            continue;
        }
        const hw_item_t *it = hw_at(i);
        uint16_t bg = st_color(it->st);
        fill_rect(x0 + 1, y0 + 1, CELL_W - 2, CELL_H - 2, bg);
        char num[4];
        snprintf(num, sizeof(num), "%02d", i);
        /* FAIL 用白字 (红底), 其余用黑字 (亮底) */
        draw_text(x0 + 4, y0 + 6, num, 2, it->st == HW_ST_FAIL ? HW_C_TEXT : HW_C_BG);
    }

    /* 页脚: 失败项编号 (最多 3 个) */
    if (hw_count(HW_ST_FAIL) == 0) {
        draw_text_centered(222, "ALL PASS", 2, HW_C_PASS);
    } else {
        char foot[32];
        size_t u = (size_t)snprintf(foot, sizeof(foot), "FAIL");
        int shown = 0;
        for (int i = 0; i < n && shown < 3; i++) {
            const hw_item_t *it = hw_at(i);
            if (it->st != HW_ST_FAIL) continue;
            u += (size_t)snprintf(foot + u, sizeof(foot) - u, " %02d", i);
            shown++;
        }
        draw_text_centered(222, foot, 2, HW_C_FAIL);
    }

    int r = hw_lcd_blit(s_fb);
    heap_caps_free(s_fb);
    s_fb = NULL;
    if (r != 0) {
        hw_warn("screen.board", "屏幕看板", "推屏失败: %d", r);
        return r;
    }

    /* 背光开 (之前一直关着, 免得花屏/随机内容被看到) */
    ledc_set_duty(HW_LEDC_SPD, HW_BL_CH, (uint32_t)HW_EXP_BL_DUTY_PCT * 1023 / 100);
    ledc_update_duty(HW_LEDC_SPD, HW_BL_CH);

    hw_ok("screen.board", "屏幕看板", "%s %dx%d 已推屏, 背光 %d%%",
          internal ? "内部RAM" : "PSRAM", SCR_W, SCR_H, HW_EXP_BL_DUTY_PCT);
    ESP_LOGI(TAG, "看板已出 (%s)", internal ? "内部RAM" : "PSRAM");
    return ESP_OK;
}
