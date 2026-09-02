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
#include "ui/screens/settings_screen.h"
#include "ui/screens/diary_screen.h"
#include "ui/common/screen_switch.h"
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

static void sim_data_timer_cb(lv_timer_t *t) {
    (void)t;
    static int s_tick = 0;
    s_tick++;

    /* 模拟温度有微小波动 */
    sim_temp = 25.3f + (s_tick % 10) * 0.1f;
    sim_hum  = 58.7f - (s_tick % 5) * 0.2f;
    sim_lux  = 320.0f + (s_tick % 20) * 15.0f;

    /* 传感器/电量数据链路已移交固件 sensor 上报路径 (home_screen_set_data
     * 已被移除), 模拟器暂不经此灌数据 — 数据接回见重构后的 home 数据接口 */
    status_bar_set_wifi(true, -45);
    status_bar_set_battery(85, 3800);

    /* 纯手动模式: 不再定时弹气泡/通知/动画, 全部由按键触发
     * (C=气泡, N=通知, 1~7=动画) */
}

/* ── 主页 screen 对象 (SDL 默认屏; 设置页关闭后切回) ── */
static lv_obj_t *s_home_scr = NULL;

/* ── 设置页退出回调: 先切回主页再销毁 (照 diary_screen.c 的"先切后删"范式) ── */
static void sim_settings_close_cb(void) {
    screen_load_full(s_home_scr);
    settings_screen_destroy();
}

/* ── 键盘快捷键: 数字键切换动画 (边沿触发) ──
 * 槽位: 0-6 数字键, 7=C, 8=N, 9=H, 10=S, 11=D, 12-15=Up/Down/Enter/Esc */
static bool s_key_prev[16];

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

    /* S: 打开/关闭设置页 (激活时注入 BACK, 根页自动触发 close_cb 回主页) */
    bool s = keys[SDL_SCANCODE_S] != 0;
    if (s && !s_key_prev[10]) {
        printf("[sim] 按键 S: %s\n",
               settings_screen_is_active() ? "关闭设置页" : "打开设置页");
        if (settings_screen_is_active()) {
            settings_screen_input(MENU_EV_BACK);
        } else if (!diary_screen_is_active()) {
            settings_screen_set_close_cb(sim_settings_close_cb);
            settings_screen_init();
        }
    }
    s_key_prev[10] = s;

    /* D: 打开日记页 (从主页直接打开; BACK 回主页) */
    bool d = keys[SDL_SCANCODE_D] != 0;
    if (d && !s_key_prev[11] && !settings_screen_is_active() && !diary_screen_is_active()) {
        printf("[sim] 按键 D: 打开日记页\n");
        diary_screen_init();
    }
    s_key_prev[11] = d;

    /* 方向键 + 回车 + ESC: 注入设置/日记页导航 (按真机 input_handler 优先级路由) */
    bool up    = keys[SDL_SCANCODE_UP] != 0;
    bool down  = keys[SDL_SCANCODE_DOWN] != 0;
    bool enter = keys[SDL_SCANCODE_RETURN] != 0;
    bool esc   = keys[SDL_SCANCODE_ESCAPE] != 0;

    if ((up && !s_key_prev[12]) || (down && !s_key_prev[13]) ||
        (enter && !s_key_prev[14]) || (esc && !s_key_prev[15])) {
        menu_event_t ev;
        if (esc)              ev = MENU_EV_BACK;
        else if (up)          ev = MENU_EV_UP;
        else if (down)        ev = MENU_EV_DOWN;
        else                  ev = MENU_EV_CONFIRM;

        if (diary_screen_is_active()) {
            printf("[sim] 按键 %s → 日记页\n", esc ? "ESC" : up ? "UP" : down ? "DOWN" : "ENTER");
            diary_screen_input(ev);
        } else if (settings_screen_is_active()) {
            printf("[sim] 按键 %s → 设置页\n", esc ? "ESC" : up ? "UP" : down ? "DOWN" : "ENTER");
            settings_screen_input(ev);
        }
    }
    s_key_prev[12] = up;
    s_key_prev[13] = down;
    s_key_prev[14] = enter;
    /* s_key_prev[15] (esc) 在退出块统一更新, 供边沿检测 */

    /* Q: 退出 (ESC 已路由给设置/日记页, 仅"连续两帧无 active 屏"时才退出)
     * ⚠ 坑: BACK 切换是同步的 — 从设置根页 ESC 回主页的同一帧里
     * is_active() 已为 false, 若此时直接判"主页 ESC 退出"会误杀进程,
     * 必须要求上一帧也无 active 屏 (s_prev_no_active). */
    static bool s_prev_no_active = true;
    bool now_active = settings_screen_is_active() || diary_screen_is_active();
    if (keys[SDL_SCANCODE_Q] ||
        (esc && !s_key_prev[15] && s_prev_no_active && !now_active)) {
        printf("[sim] 退出\n");
        exit(0);
    }
    s_key_prev[15] = esc;
    s_prev_no_active = !now_active;
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

    /* 调试输出: 重定向到管道时也即时可见 (kill 不丢日志) */
    setvbuf(stdout, NULL, _IONBF, 0);

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
    home_screen_init();
    /* 保存主页 screen 对象 (SDL 默认屏; 设置页关闭后 screen_load_full 切回) */
    s_home_scr = lv_screen_active();

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
    printf("[sim]   S                — 打开/关闭设置页\n");
    printf("[sim]   D                — 打开日记页 (需 simdata/diary/ 下有 HTML)\n");
    printf("[sim]   ↑/↓/回车/ESC     — 设置/日记页导航 (与真机 input_handler 同语义)\n");
    printf("[sim]   Q                — 退出 (主页时 ESC 亦可)\n");
    printf("[sim]   鼠标滚轮          — 模拟编码器\n");

    /* 7. 主循环 */
    const char *sim_timeout = getenv("SIM_TIMEOUT_MS");
    while (1) {
        uint32_t delay_ms = lv_timer_handler();
        if (delay_ms > 100) delay_ms = 100;  /* cap at 100ms for SDL responsiveness */
        SDL_Delay(delay_ms > 1 ? delay_ms : 1);

        /* SIM_TIMEOUT_MS: 自动退出 (CI/自动化冒烟验证, 日志需落盘:
         * 直接 kill 会丢 stdio 缓冲, 这里走正常退出路径) */
        if (sim_timeout && SDL_GetTicks() > (Uint32)atoi(sim_timeout)) {
            fflush(NULL);
            exit(0);
        }
    }

    return 0;
}
