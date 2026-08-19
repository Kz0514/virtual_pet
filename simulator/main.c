/**
 * @file main.c
 * @brief LVGL PC 模拟器入口 — SDL2 后端, 复用 main/ui/ 代码
 *
 * 编译: mkdir build && cd build && cmake .. && make
 * 运行: ./lvgl_simulator  (需在 simulator/ 目录下运行)
 */

#include "lvgl.h"
#include "src/drivers/sdl/lv_sdl_window.h"
#include "src/drivers/sdl/lv_sdl_mouse.h"
#include "src/drivers/sdl/lv_sdl_keyboard.h"
#include <SDL.h>

/* ── 项目的 LVGL UI 代码 (直接引用, 不动原文件) ── */
#include "ui/screens/home_screen.h"
#include "ui/widgets/pet_avatar.h"
#include "ui/widgets/status_bar.h"
#include "ui/widgets/chat_bubble.h"
#include "ui/widgets/notify_overlay.h"
#include "ui/widgets/brightness_bar.h"
#include "ui/fonts/font_loader.h"

#include <stdio.h>
#include <stdlib.h>

/* ── 模拟数据定时器: 给 UI 灌入假数据验证显示效果 ── */
static float sim_temp = 25.3f;
static float sim_hum = 58.7f;
static float sim_lux = 320.0f;
static int sim_counter = 0;

static void sim_data_timer_cb(lv_timer_t *t) {
    (void)t;
    sim_counter++;

    /* 模拟温度有微小波动 */
    sim_temp = 25.3f + (sim_counter % 10) * 0.1f;
    sim_hum  = 58.7f - (sim_counter % 5) * 0.2f;
    sim_lux  = 320.0f + (sim_counter % 20) * 15.0f;

    home_screen_set_data(
        sim_temp, sim_hum, sim_lux,
        3800, 85,             /* 电池电压/百分比 */
        32.5f,                /* 电池温度 */
        -120,                 /* 放电电流 mA */
        680, 800,             /* 剩余/满容量 mAh */
        true, "192.168.1.100" /* WiFi */
    );

    status_bar_set_wifi(true, -45);
    status_bar_set_battery(85, 3800);

    /* 每10秒显示一条聊天消息做演示 */
    if (sim_counter == 20) {
        chat_bubble_show("你好! 我是你的虚拟宠物~", 5000);
    }
    if (sim_counter == 40) {
        notify_show(NOTIFY_INFO, "固件已是最新版本 v2.0", 3000);
    }
    if (sim_counter == 60) {
        chat_bubble_show("今天天气不错, 出去走走吧! |p1000 记得带伞哦~", 5000);
    }
    if (sim_counter == 80) {
        pet_avatar_play(PET_ANIM_HAPPY);
    }
    if (sim_counter == 90) {
        pet_avatar_play(PET_ANIM_EXCITED);
    }
    if (sim_counter == 100) {
        pet_avatar_play(PET_ANIM_SLEEPY);
    }
    if (sim_counter == 110) {
        notify_show(NOTIFY_WARN, "电量低于 20%, 请充电", 4000);
        pet_avatar_play(PET_ANIM_SAD);
    }
    if (sim_counter >= 115 && sim_counter < 120) {
        /* 回到 idle */
        pet_avatar_play(PET_ANIM_IDLE);
        sim_counter = 0;
    }
}

/* ── 键盘快捷键: 数字键切换动画 (边沿触发) ── */
static bool s_key_prev[12];

static void key_shortcut_timer_cb(lv_timer_t *t) {
    (void)t;
    const Uint8 *keys = SDL_GetKeyboardState(NULL);

    static const pet_anim_t anim_map[7] = {
        PET_ANIM_IDLE, PET_ANIM_HAPPY, PET_ANIM_SAD, PET_ANIM_EXCITED,
        PET_ANIM_SLEEPY, PET_ANIM_EATING, PET_ANIM_SURPRISED
    };

    /* 数字键 1~7: 切换动画 */
    for (int i = 0; i < 7; i++) {
        bool pressed = keys[SDL_SCANCODE_1 + i] != 0;
        if (pressed && !s_key_prev[i]) {
            printf("[sim] 切换动画: %d\n", i);
            pet_avatar_play(anim_map[i]);
        }
        s_key_prev[i] = pressed;
    }

    /* C: 显示聊天气泡 */
    bool c = keys[SDL_SCANCODE_C] != 0;
    if (c && !s_key_prev[7]) {
        chat_bubble_show("你好呀! 我是你的虚拟宠物, 今天也要加油哦~ |p800 喵~", 6000);
    }
    s_key_prev[7] = c;

    /* N: 显示通知 */
    bool n = keys[SDL_SCANCODE_N] != 0;
    if (n && !s_key_prev[8]) {
        notify_show(NOTIFY_INFO, "这是一条测试通知", 3000);
    }
    s_key_prev[8] = n;

    /* H: 切换 WiFi 状态 */
    bool h = keys[SDL_SCANCODE_H] != 0;
    if (h && !s_key_prev[9]) {
        static bool wifi_on = false;
        wifi_on = !wifi_on;
        status_bar_set_wifi(wifi_on, wifi_on ? -45 : 0);
        printf("[sim] WiFi: %s\n", wifi_on ? "on" : "off");
    }
    s_key_prev[9] = h;

    /* Q / ESC: 退出 */
    if (keys[SDL_SCANCODE_Q] || keys[SDL_SCANCODE_ESCAPE]) {
        printf("[sim] 退出\n");
        exit(0);
    }
}

/* ── 字体调试: 用 LVGL API 检查关键字符是否可解析 ── */
static void font_debug_check(const char *tag) {
    const lv_font_t *f = FONT_ZH;
    if (!f) { printf("[font] %s: 无字体\n", tag); return; }
    static const struct { uint32_t code; const char *name; } chars[] = {
        {0x4F60, "你"}, {0x55B5, "喵"}, {0x54E6, "哦"}, {0x0021, "!"},
        {0x5417, "吗"}, {0x554A, "啊"}, {0x597D, "好"}, {0x7684, "的"},
        {0x4E00, "一"}, {0x6211, "我"}, {0x4ECA, "今"}, {0x5929, "天"},
    };
    printf("[font] %s: 字体检查:\n", tag);
    for (unsigned i = 0; i < sizeof(chars) / sizeof(chars[0]); i++) {
        lv_font_glyph_dsc_t g;
        bool ok = lv_font_get_glyph_dsc(f, &g, chars[i].code, 0);
        if (ok)
            printf("[font]   U+%04X %s: OK  box=%dx%d adv=%d\n",
                   chars[i].code, chars[i].name, g.box_w, g.box_h, g.adv_w);
        else
            printf("[font]   U+%04X %s: MISSING\n", chars[i].code, chars[i].name);
    }

    /* 转储运行时 cmap 状态 — 揭示加载器实际读到的映射表 */
    const lv_font_fmt_txt_dsc_t *fdsc = (const lv_font_fmt_txt_dsc_t *)f->dsc;
    if (fdsc) {
        printf("[font] dsc: cmap_num=%d bpp=%d bitmap_format=%d stride=%d\n",
               fdsc->cmap_num, fdsc->bpp, fdsc->bitmap_format, fdsc->stride);
        for (int i = 0; i < fdsc->cmap_num && i < 12; i++) {
            const lv_font_fmt_txt_cmap_t *cm = &fdsc->cmaps[i];
            printf("[font]   cmap[%d]: type=%d range=U+%04X len=%d gid_start=%d list_len=%d\n",
                   i, cm->type, cm->range_start, cm->range_length,
                   cm->glyph_id_start, cm->list_length);
            if (cm->unicode_list) {
                printf("[font]     ulist head: ");
                for (int k = 0; k < 8 && k < cm->list_length; k++)
                    printf("%04X ", cm->unicode_list[k]);
                printf("\n");
            }
        }

        /* 转储字形的原始位图 (高半字节优先解码), 验证像素布局 */
        printf("[font] 位图转储 (4bpp, 高nibble优先):\n");
        const uint32_t dump_chars[] = { 0x4E00 /*一*/, 0x0021 /*!*/, 0x6211 /*我*/,
                                        0x55B5 /*喵 odd宽*/, 0x54E6 /*哦 odd宽*/ };
        for (unsigned d = 0; d < sizeof(dump_chars) / sizeof(dump_chars[0]); d++) {
            lv_font_glyph_dsc_t g;
            if (!lv_font_get_glyph_dsc(f, &g, dump_chars[d], 0)) continue;
            const lv_font_fmt_txt_glyph_dsc_t *gd =
                &fdsc->glyph_dsc[g.gid.index];
            int row_bytes = (gd->box_w + 1) / 2;  /* 4bpp: 2px/byte */
            const uint8_t *bmp = &fdsc->glyph_bitmap[gd->bitmap_index];
            printf("[font]  U+%04X box=%dx%d format=%d:\n",
                   dump_chars[d], gd->box_w, gd->box_h, (int)g.format);
            for (int y = 0; y < gd->box_h; y++) {
                printf("[font]   ");
                for (int x = 0; x < gd->box_w; x++) {
                    uint8_t px = (x & 1)
                        ? (bmp[y * row_bytes + x / 2] & 0x0F)
                        : (bmp[y * row_bytes + x / 2] >> 4);
                    printf("%X", px);
                }
                printf("\n");
            }
        }
    } else {
        printf("[font] 无 fmt_txt dsc\n");
    }
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    /* 1. 初始化 LVGL */
    lv_init();

    /* 2. 创建 SDL 窗口 (240×240, 2x 缩放便于观看) */
    lv_display_t *disp = lv_sdl_window_create(240, 240);
    if (!disp) {
        fprintf(stderr, "FATAL: 无法创建 SDL 窗口 (SDL2 安装了吗?)\n");
        return 1;
    }
    lv_sdl_window_set_zoom(disp, 2.0f);
    lv_sdl_window_set_title(disp, "Virtualpet Simulator");

    /* 3. 创建鼠标和键盘输入设备 */
    lv_sdl_mouse_create();
    lv_sdl_keyboard_create();

    /* 4. 初始化字体 (从 spiffs/zh.bin 加载) */
    font_loader_init();
    font_debug_check("启动");

    /* 5. 创建所有 UI 组件 */
    printf("[sim] 创建 home_screen...\n");
    home_screen_create();

    printf("[sim] 初始化 pet_avatar...\n");
    if (pet_avatar_init() != 0) {
        printf("[sim] WARNING: pet_avatar 初始化失败 (动画文件缺失?), 继续运行\n");
    }

    printf("[sim] 初始化 status_bar...\n");
    status_bar_init();

    printf("[sim] 初始化 chat_bubble...\n");
    chat_bubble_init();

    printf("[sim] 初始化 notify_overlay...\n");
    notify_overlay_init();

    printf("[sim] 初始化 brightness_bar...\n");
    brightness_bar_init();

    /* 6. 模拟数据定时器: 每500ms更新一次传感器数据 */
    lv_timer_create(sim_data_timer_cb, 500, NULL);

    /* 7. 键盘快捷键轮询: 每50ms检查一次 */
    lv_timer_create(key_shortcut_timer_cb, 50, NULL);

    printf("[sim] === 模拟器就绪 ===\n");
    printf("[sim] 键盘快捷键 (需窗口在前台):\n");
    printf("[sim]   1/2/3/4/5/6/7  — 切换动画 (idle/happy/sad/excited/sleepy/eating/surprised)\n");
    printf("[sim]   C                — 显示聊天气泡\n");
    printf("[sim]   N                — 显示通知\n");
    printf("[sim]   H                — 切换 WiFi 状态\n");
    printf("[sim]   Q/ESC            — 退出\n");
    printf("[sim]   鼠标滚轮          — 模拟编码器\n");

    /* 7. 主循环 */
    while (1) {
        uint32_t delay_ms = lv_timer_handler();
        if (delay_ms > 100) delay_ms = 100;  /* cap at 100ms for SDL responsiveness */
        SDL_Delay(delay_ms > 1 ? delay_ms : 1);
    }

    return 0;
}
