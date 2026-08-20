/**
 * @file settings_screen.c
 * @brief 设置界面 v2 — 子页面模型, 交互由 input_handler 注入
 *
 * 菜单结构 (无展开状态, 确认父项进入子页面):
 *   根页:   显示 → / 选择逻辑(调值 点击/滑动, 实时生效) / 关于 →
 *   显示页: 亮度(调值 1-100 步长5, 实时生效)
 *           自动息屏(调值 预设 30/60/90/120/300s, 实时生效)
 *           主界面亮度条(开关)
 *   关于页: 固件版本(只读, 不可选中)
 *
 * 两种模式:
 *   列表模式 — UP/DOWN 移动选择, CONFIRM 进入子页/调值/切换, BACK 回上级
 *   调值模式 — UP/DOWN 调值实时生效, CONFIRM/BACK 退出调值
 *
 * UI 要点: 顶栏 "← HOME/齿轮 标题"; 箭头 LV_SYMBOL_RIGHT 白色靠右缘,
 * 仅父项(IT_SUBPAGE)显示, 标识可进入子页; 叶子项数值贴右缘;
 * IT_TOGGLE 项用 lv_switch 显示状态; IT_INFO 项只读不可选中;
 * 选中状态由行背景(浅灰)/调值(浅蓝)表达。
 *
 * 本文件不轮询触摸、不消费手势 — 旧版双重消费/分区错位/索引混用问题
 * 从结构上消除。
 */
#include "settings_screen.h"
#include "font_loader.h"
#include "brightness_bar.h"
#include "config_mgr.h"
#include "time_manager.h"
#include "tm6604.h"
#include "usb_storage.h"
#include "ota_client.h"
#include "esp_log.h"
#include "esp_system.h"      /* esp_restart — 设置页重启设备 */
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "ff.h"       /* f_getfree — 数据分区空闲空间 */
#include <stdio.h>
#include <string.h>

static const char *TAG = "settings";

/* NVS 键 (config_mgr, ns "settings") — 与 brightness_bar/main.c 共用 */
#define CFG_KEY_BRI     "bri"
#define CFG_KEY_OFF_S   "off_s"
#define CFG_KEY_BAR_EN  "bri_bar_en"
#define CFG_KEY_NAV     "nav_mode"   /* 与 input_handler 共用: 0=点击 1=滑动 */
#define CFG_KEY_TZ_AUTO "tz_auto"    /* 与 time_manager 共用: 1=自动 0=手动 */
#define CFG_KEY_TZ_MAN  "tz_manual_sec"

#define MAX_VISIBLE  5          /* 可视行数 */
#define ITEM_H       42         /* 行高: 28 顶栏 + 5×42 = 238 ≤ 240 */
#define VIBE_THROTTLE_MS 100

/* main.c 导出: 息屏时长调节实时生效 */
extern void main_screen_set_off_timeout_s(uint32_t off_s);

/* ── 页面与菜单定义 ── */
typedef enum { PAGE_ROOT = 0, PAGE_DISPLAY, PAGE_TZ, PAGE_STORAGE, PAGE_ABOUT, PAGE_COUNT } page_id_t;

typedef enum {
    IT_SUBPAGE,     /* CONFIRM → 进入子页面 */
    IT_ADJUST,      /* CONFIRM → 调值模式; UP/DOWN 实时调值 */
    IT_TOGGLE,      /* CONFIRM → 切换开/关 */
    IT_INFO,        /* 只读 */
} item_type_t;

typedef struct {
    const char *label;
    item_type_t type;
    page_id_t   subpage;        /* IT_SUBPAGE: 目标页 */
} item_t;

static const item_t s_root_items[] = {
    { "显示",     IT_SUBPAGE, PAGE_DISPLAY },
    { "时区",     IT_SUBPAGE, PAGE_TZ },
    { "存储",     IT_SUBPAGE, PAGE_STORAGE },
    { "选择逻辑", IT_ADJUST,  0 },
    { "关于",     IT_SUBPAGE, PAGE_ABOUT },
};
static const item_t s_display_items[] = {
    { "亮度",         IT_ADJUST, 0 },
    { "自动息屏",     IT_ADJUST, 0 },
    { "主界面亮度条", IT_TOGGLE, 0 },
};
static const item_t s_tz_items[] = {
    { "自动时区", IT_TOGGLE, 0 },
    { "时区偏移", IT_ADJUST, 0 },
};
static const item_t s_storage_items[] = {
    { "U盘模式",     IT_TOGGLE, 0 },
    { "数据分区",     IT_INFO,   0 },
    { "格式化存储",   IT_ADJUST, 0 },
};
static const item_t s_about_items[] = {
    { "固件版本", IT_INFO, 0 },
    { "检查更新", IT_ADJUST, 0 },   /* 确认执行 — 手动触发 OTA 检查 (主循环同步执行) */
    { "重启设备", IT_ADJUST, 0 },   /* 确认执行 — 无实体按键设备的自救口 (OTA 后同场景) */
};

static const char *s_page_title[PAGE_COUNT] = { "设置", "显示", "时区", "存储", "关于" };

/* 手动时区偏移档位 (秒) — UTC-12 ~ UTC+14 整小时 */
static const int32_t s_tz_presets[] = {
    -43200, -39600, -36000, -32400, -28800, -25200, -21600, -18000,
    -14400, -10800, -7200, -3600, 0, 3600, 7200, 10800, 14400,
    18000, 21600, 25200, 28800, 32400, 36000, 39600, 43200, 46800, 50400,
};
#define TZ_PRESET_CNT  (sizeof(s_tz_presets) / sizeof(s_tz_presets[0]))
#define TZ_PRESET_DEF  28800   /* UTC+8 */

/* 自动息屏预设 (秒) — off_s; dim = off−30s 下限 15s (main.c 内实现) */
static const uint32_t s_off_presets[] = { 30, 60, 90, 120, 300 };
#define OFF_PRESET_CNT  (sizeof(s_off_presets) / sizeof(s_off_presets[0]))
#define OFF_PRESET_DEF  90

/* ── 运行时状态 ── */
static lv_obj_t  *s_scr   = NULL;
static lv_obj_t  *s_icons = NULL;
static lv_obj_t  *s_title = NULL;
static lv_obj_t  *s_rows[MAX_VISIBLE];
static lv_obj_t  *s_row_ind[MAX_VISIBLE];
static lv_obj_t  *s_row_lbl[MAX_VISIBLE];
static lv_obj_t  *s_row_val[MAX_VISIBLE];
static lv_obj_t  *s_row_sw[MAX_VISIBLE];   /* IT_TOGGLE 项的开关 */
static bool       s_active = false;

static page_id_t  s_page = PAGE_ROOT;
static int        s_sel = 0;            /* 本页选中索引 */
static bool       s_adjusting = false;  /* 调值模式 */
static uint32_t   s_last_vib = 0;
static void     (*s_close_cb)(void) = NULL;

/* ── 设置页内嵌提示框 (1.0.227) ──
 * OTA 检查等设置页操作的结果提示。挂设置页 screen, 离开设置页即不可见;
 * 替代全局 notify_overlay (仅保留主页/对话场景)。线程安全:
 * 业务任务写标志, LVGL 定时器 (100ms) 消费。 */
static lv_obj_t      *s_toast;
static lv_obj_t      *s_toast_lbl;
static lv_timer_t    *s_toast_timer;   /* 标志轮询 */
static lv_timer_t    *s_toast_hide;    /* auto-hide 定时器 (单发) */
static volatile bool  s_toast_req;
static char           s_toast_text[128];
static uint32_t       s_toast_auto_ms;
static notify_type_t  s_toast_type;

static void toast_hide_cb(lv_timer_t *t);
static void toast_poll_cb(lv_timer_t *t);

/* ── 页面项表访问 ── */
static const item_t *page_items(page_id_t p, int *count)
{
    switch (p) {
    case PAGE_DISPLAY: *count = 3; return s_display_items;
    case PAGE_TZ:      *count = 2; return s_tz_items;
    case PAGE_STORAGE: *count = 3; return s_storage_items;
    case PAGE_ABOUT:   *count = 3; return s_about_items;
    default:           *count = 5; return s_root_items;
    }
}

/* ── 偏移秒 → "UTC+8" 显示串 ── */
static void tz_fmt(int32_t off, char *buf, size_t len)
{
    int h = (int)(off / 3600);
    int m = (int)((off % 3600) / 60);
    if (m == 0) snprintf(buf, len, "UTC%+d", h);
    else        snprintf(buf, len, "UTC%+d:%02d", h, (m < 0) ? -m : m);
}

/* IT_INFO 为只读项: 不可选中 (无高亮 / 无 ">" / 选择跳过) */
static bool item_selectable(const item_t *it) { return it->type != IT_INFO; }

static int first_selectable(page_id_t p)
{
    int count;
    const item_t *items = page_items(p, &count);
    for (int i = 0; i < count; i++) {
        if (item_selectable(&items[i])) return i;
    }
    return -1;
}

/* 数据分区信息: U盘模式中 → "U盘模式中"; 未挂载 → "未挂载"; 挂载态 → 总容量 + 空闲空间。
 *
 * 总/空闲**都取自 FatFS 卷本身** (fs->n_fatent/csize/ssize + f_getfree) —
 * 与 Windows 资源管理器一致。卷总大小 = 分区大小减去 WL 磨损均衡保留区
 * (本设备 1MB 分区 → 卷 724K), 总容量小于分区是正常现象; 曾用
 * usb_storage_capacity (WL 容量) 显示总容量 → 与 Windows 差 ~300K。
 * 单位用卷自身扇区字节 (fs->ssize)。
 * 探测: 走 usb_storage 互斥 API — **只探测 WL 注册盘号** (IDF diskio 对未
 * 注册槽无守卫, 全盘扫描必崩 1.0.216) + 与 repair 重建互斥 (中间态 f_getfree
 * 返回 FRESULT=12, 1.0.218) */
static void storage_info_text(char *buf, size_t len)
{
    if (usb_storage_is_active()) { snprintf(buf, len, "U盘模式中"); return; }
    if (!usb_storage_data_mounted()) { snprintf(buf, len, "未挂载"); return; }

    /* 走 usb_storage 的互斥探测 — 直接 f_getfree 会在 repair 重建中间态
     * 看到 FatFs 空卷 (FRESULT=12), 误报"-" (1.0.218) */
    uint32_t total_kb = 0, free_kb = 0;
    if (!usb_storage_probe_data(&total_kb, &free_kb)) {
        ESP_LOGW(TAG, "数据分区探测失败 — 挂载态与 FatFS 实际状态不一致, "
                      "可尝试设置页\"格式化存储\"");
        snprintf(buf, len, "—");
        return;
    }
    snprintf(buf, len, "总 %luK 空闲 %luK", (unsigned long)total_kb, (unsigned long)free_kb);
}

/* ── 数值文本 ── */
static void get_value_text(page_id_t page, int idx, char *buf, size_t len)
{
    buf[0] = '\0';
    if (page == PAGE_ROOT && idx == 3) {           /* 选择逻辑 (0=显示 1=时区 2=存储 3=选择逻辑 4=关于) */
        snprintf(buf, len, "%s",
                 config_get_u32(CFG_KEY_NAV, 1) ? "滑动" : "点击");
        return;
    }
    if (page == PAGE_DISPLAY) {
        switch (idx) {
        case 0:  /* 亮度 */
            snprintf(buf, len, "%u%%", (unsigned)brightness_bar_get());
            break;
        case 1:  /* 自动息屏 */
            snprintf(buf, len, "%lus",
                     (unsigned long)config_get_u32(CFG_KEY_OFF_S, OFF_PRESET_DEF));
            break;
        case 2:  /* 主界面亮度条 */
            snprintf(buf, len, "%s",
                     config_get_u32(CFG_KEY_BAR_EN, 1) ? "开" : "关");
            break;
        }
    } else if (page == PAGE_TZ) {
        switch (idx) {
        case 0:  /* 自动时区: 值文本不显示 (开关表达状态) */
            break;
        case 1:  /* 时区偏移: 自动开启时显示"自动", 关闭时显示 UTC±H */
            if (config_get_u32(CFG_KEY_TZ_AUTO, 1)) {
                snprintf(buf, len, "自动");
            } else {
                tz_fmt((int32_t)config_get_u32(CFG_KEY_TZ_MAN, TZ_PRESET_DEF),
                       buf, len);
            }
            break;
        }
    } else if (page == PAGE_STORAGE) {
        switch (idx) {
        case 0:  /* U盘模式: 开关表达状态, 无值文本 */
            break;
        case 1:  /* 数据分区 */
            storage_info_text(buf, len);
            break;
        case 2:  /* 格式化存储: 调值中提示执行方式 (确认后重启执行) */
            if (s_adjusting) {
                if (usb_storage_is_active())
                    snprintf(buf, len, "先关闭U盘模式");
                else
                    snprintf(buf, len, "确认重启格式化");
            }
            break;
        }
    } else if (page == PAGE_ABOUT) {
        switch (idx) {
        case 0: {   /* 固件版本 */
            const esp_app_desc_t *app = esp_app_get_description();
            snprintf(buf, len, "%s", app->version);
            break;
        }
        case 1:     /* 检查更新: 二次确认提示仅显示在被调的那一行
                     * (s_adjusting 是全局的 — 曾漏判 s_sel, 导致确认检查时
                     * 下方的重启设备行也冒出"再按确认重启") */
            if (s_adjusting && s_sel == 1) snprintf(buf, len, "再按确认检查");
            break;
        case 2:     /* 重启设备: 同上 */
            if (s_adjusting && s_sel == 2) snprintf(buf, len, "再按确认重启");
            break;
        }
    }
}

/* ── 开关项状态 (IT_TOGGLE 用 lv_switch 显示) ── */
static bool get_toggle_state(page_id_t page, int idx)
{
    if (page == PAGE_DISPLAY && idx == 2) {    /* 主界面亮度条 */
        return config_get_u32(CFG_KEY_BAR_EN, 1) != 0;
    }
    if (page == PAGE_TZ && idx == 0) {         /* 自动时区 */
        return config_get_u32(CFG_KEY_TZ_AUTO, 1) != 0;
    }
    if (page == PAGE_STORAGE && idx == 0) {    /* U盘模式 (运行时状态, 不入 NVS) */
        return usb_storage_is_active();
    }
    return false;
}

/* ── 导航震动 (100ms 节流; 设置页无摇/敲/摸头, 不自激) ── */
static void nav_vibe(void)
{
    uint32_t now = xTaskGetTickCount();
    if (now - s_last_vib >= pdMS_TO_TICKS(VIBE_THROTTLE_MS)) {
        s_last_vib = now;
        /* 本马达 ~50% 占空比才起振, 整数百分比粒度不够 —
         * 537/1023 ≈ 52.5%, 等效约 4% 震感 (用户 2026-08-18 标定) */
        tm6604_vibrate_raw(537, 40);
    }
}

/* ── 行视觉状态缓存 ──
 * 防撕裂关键: LVGL 样式 setter / lv_label_set_text 即使值相同也会
 * invalidate 对象区域, 5 个相邻行会被合并成一个大脏区 → 局部缓冲
 * (240×48) 下每次导航要刷 5-6 个 SPI 分片, 与面板异步扫描交叉 → 撕裂。
 * 缓存每行"已应用"的视觉状态, 无变化的行完全不触碰 LVGL —
 * 纯选择移动只失效新旧选中两行, 调值只失效数值一行。
 * (lv_obj_add_flag/clear_flag 本身已有"标志已置位则早退", 无需缓存) */
typedef struct {
    int8_t bg_state;      /* -1=未缓存; 0=普通 1=选中(浅灰) 2=调值中(浅蓝) */
    int8_t row_kind;      /* -1=未缓存; 0=普通项 1=开关项 */
    bool   sw_checked;    /* 开关选中态 */
    bool   has_arrow;     /* 父项箭头 (LV_SYMBOL_RIGHT) */
    char   val[40];       /* 数值文本 (开关项缓存为 "") ≥ version[32] */
} row_cache_t;

static row_cache_t s_cache[MAX_VISIBLE];
static int s_hdr_page = -1;     /* 顶栏图标/标题已应用到的页 (-1=未应用) */

static void cache_invalidate_all(void)
{
    for (int i = 0; i < MAX_VISIBLE; i++) {
        s_cache[i].bg_state   = -1;
        s_cache[i].row_kind   = -1;
        s_cache[i].sw_checked = false;
        s_cache[i].has_arrow  = false;
        s_cache[i].val[0]     = '\0';
    }
    s_hdr_page = -1;
}

/* ── 刷新: 顶栏图标/标题 + 列表 (只写入有变化的部分, 见缓存说明) ── */
static void refresh(void)
{
    /* 顶栏只在切页时变化 */
    if (s_hdr_page != (int)s_page) {
        s_hdr_page = (int)s_page;
        /* 根页返回主页 = HOME; 子页返回上级 = 设置页 = 齿轮 */
        lv_label_set_text(s_icons,
            (s_page == PAGE_ROOT) ? LV_SYMBOL_LEFT " " LV_SYMBOL_HOME
                                  : LV_SYMBOL_LEFT " " LV_SYMBOL_SETTINGS);
        lv_label_set_text(s_title, s_page_title[s_page]);
    }

    int count;
    const item_t *items = page_items(s_page, &count);
    int show = (count < MAX_VISIBLE) ? count : MAX_VISIBLE;

    for (int i = 0; i < MAX_VISIBLE; i++) {
        if (i >= show) {
            lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);

        const item_t *it = &items[i];
        bool selected = (i == s_sel);
        bool adj_here = (s_adjusting && selected);
        bool is_toggle = (it->type == IT_TOGGLE);

        int8_t bg_state  = adj_here ? 2 : (selected ? 1 : 0);
        bool   has_arrow = (it->type == IT_SUBPAGE);
        bool   sw_on     = is_toggle && get_toggle_state(s_page, i);
        char vbuf[40];
        if (is_toggle) vbuf[0] = '\0';
        else get_value_text(s_page, i, vbuf, sizeof(vbuf));

        /* 与缓存比对: 无变化则跳过, 不产生任何失效 */
        row_cache_t *c = &s_cache[i];
        if (c->bg_state == bg_state &&
            c->row_kind == (is_toggle ? 1 : 0) &&
            c->sw_checked == sw_on &&
            c->has_arrow == has_arrow &&
            strcmp(c->val, vbuf) == 0) {
            continue;
        }

        /* 行背景: 普通/选中(浅灰)/调值中(浅蓝) */
        lv_obj_set_style_bg_color(s_rows[i],
            (bg_state == 2) ? lv_color_hex(0x4a90d9) :
            (bg_state == 1) ? lv_color_hex(0x333344) :
                              lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_rows[i],
            (bg_state == 0) ? LV_OPA_TRANSP : LV_OPA_COVER, 0);

        lv_label_set_text(s_row_lbl[i], it->label);

        if (is_toggle) {
            /* 开关项: lv_switch 显示状态, 不显示数值与 ">" (避免挤撞) */
            lv_obj_clear_flag(s_row_sw[i], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_row_val[i], "");
            lv_label_set_text(s_row_ind[i], "");
            if (sw_on) lv_obj_add_state(s_row_sw[i], LV_STATE_CHECKED);
            else       lv_obj_clear_state(s_row_sw[i], LV_STATE_CHECKED);
        } else {
            lv_obj_add_flag(s_row_sw[i], LV_OBJ_FLAG_HIDDEN);

            /* 箭头只出现在父项 (含子页面) 上, 标识"可进入"; 叶子项无箭头。
             * 选中状态由行背景高亮表达 */
            lv_label_set_text(s_row_ind[i], has_arrow ? LV_SYMBOL_RIGHT : "");
            lv_label_set_text(s_row_val[i], vbuf);
            /* 无箭头的叶子项, 数值贴右缘; 父项给箭头留位 (数值本身为空) */
            lv_obj_align(s_row_val[i], LV_ALIGN_RIGHT_MID,
                         has_arrow ? -28 : -10, 0);
            /* 调值中的数值用白色 (浅蓝底上突出) */
            lv_obj_set_style_text_color(s_row_val[i],
                adj_here ? lv_color_hex(0xffffff) : lv_color_hex(0x888888), 0);
        }

        /* 应用完毕 → 更新缓存 */
        c->bg_state   = bg_state;
        c->row_kind   = is_toggle ? 1 : 0;
        c->sw_checked = sw_on;
        c->has_arrow  = has_arrow;
        memcpy(c->val, vbuf, sizeof(c->val));
    }
}

/* ── 列表模式: 移动选择 (跳过不可选项, 如 IT_INFO) ── */
static void move_sel(int dir)
{
    int count;
    const item_t *items = page_items(s_page, &count);
    int next = s_sel;
    do {
        next += dir;
    } while (next >= 0 && next < count && !item_selectable(&items[next]));
    if (next >= 0 && next < count && next != s_sel) {
        /* 新旧选中行各自失效会是两个脏区、两次 SPI 突发 —
         * 先把两行的外接矩形整体失效: lv_refr_join_area 会把
         * 重叠区域合并成单个脏区 → 一次渲染、一个 SPI 突发,
         * 两行同帧更新, 减少与面板异步扫描交叉的撕裂窗口 */
        int a = (s_sel < next) ? s_sel : next;
        int b = (s_sel < next) ? next : s_sel;
        lv_area_t span;
        lv_obj_get_coords(s_rows[a], &span);
        lv_area_t lo;
        lv_obj_get_coords(s_rows[b], &lo);
        span.y2 = lo.y2;
        lv_obj_invalidate_area(s_scr, &span);

        s_sel = next;
        refresh();
        nav_vibe();
    }
}

/* ── 调值模式: 数值变更 (实时生效 + 持久化) ── */
static void adjust_step(int dir)
{
    if (s_page == PAGE_STORAGE) return;        /* 格式化存储: 确认执行, 无调值 */
    if (s_page == PAGE_ABOUT)   return;        /* 重启设备: 确认执行, 无调值 */
    if (s_page == PAGE_ROOT && s_sel == 3) {   /* 选择逻辑: 点击↔滑动 */
        uint32_t m = config_get_u32(CFG_KEY_NAV, 1) ? 0 : 1;   /* 默认 1 (滑动), 与显示/input_handler 一致 */
        config_set_u32(CFG_KEY_NAV, m);        /* input_handler 每事件读缓存 */
        refresh();
        nav_vibe();
        return;
    }
    if (s_page == PAGE_TZ) {
        switch (s_sel) {
        case 1: {   /* 时区偏移: 档位循环 (自动开启时置灰不生效) */
            if (config_get_u32(CFG_KEY_TZ_AUTO, 1)) return;  /* 自动模式下锁定 */
            int32_t cur = (int32_t)config_get_u32(CFG_KEY_TZ_MAN, TZ_PRESET_DEF);
            int idx = 0;
            for (int i = 0; i < (int)TZ_PRESET_CNT; i++) {
                if (s_tz_presets[i] == cur) { idx = i; break; }
                if (s_tz_presets[i] < cur) idx = i;   /* 就近向下取档 */
            }
            idx += dir;
            if (idx < 0) idx = 0;
            if (idx >= (int)TZ_PRESET_CNT) idx = TZ_PRESET_CNT - 1;
            int32_t preset = s_tz_presets[idx];
            time_manager_apply_tz(preset);     /* 立即生效 */
            config_set_u32(CFG_KEY_TZ_MAN, (uint32_t)preset);
            break;
        }
        default:
            return;
        }
        refresh();
        nav_vibe();
        return;
    }
    if (s_page != PAGE_DISPLAY) return;

    switch (s_sel) {
    case 0: {   /* 亮度 1-100, 步长 5 */
        int v = (int)brightness_bar_get() + dir * 5;
        if (v < 1) v = 1;
        if (v > 100) v = 100;
        brightness_bar_set((uint8_t)v);        /* 背光立即生效 */
        config_set_u32(CFG_KEY_BRI, (uint32_t)v);
        break;
    }
    case 1: {   /* 自动息屏: 预设循环 */
        uint32_t cur = config_get_u32(CFG_KEY_OFF_S, OFF_PRESET_DEF);
        int idx = 0;
        for (int i = 0; i < (int)OFF_PRESET_CNT; i++) {
            if (s_off_presets[i] == cur) { idx = i; break; }
            if (s_off_presets[i] < cur) idx = i;   /* 就近向下取档 */
        }
        idx += dir;
        if (idx < 0) idx = 0;
        if (idx >= (int)OFF_PRESET_CNT) idx = OFF_PRESET_CNT - 1;
        uint32_t preset = s_off_presets[idx];
        main_screen_set_off_timeout_s(preset); /* dim/off 时序立即生效 */
        config_set_u32(CFG_KEY_OFF_S, preset);
        break;
    }
    default:
        return;
    }
    refresh();
    nav_vibe();
}

/* ── CONFIRM ── */
static void do_confirm(void)
{
    if (s_adjusting) {          /* 调值中: 确认 = 退出调值 (存储页=执行格式化, 关于页=检查更新/重启) */
        if (s_page == PAGE_STORAGE && s_sel == 2) {
            /* 重启后格式化: 写 NVS 标志 → esp_restart → boot 整区擦除 →
             * 组件自动格式化。运行时不再碰 FatFS (1.0.216-217 失败教训) */
            if (usb_storage_request_format() != ESP_OK)
                ESP_LOGW(TAG, "格式化请求失败 — 请确认不在 U盘模式, 详见串口日志");
        } else if (s_page == PAGE_ABOUT && s_sel == 1) {
            /* 检查更新: 置标志, 由主循环同步执行 (与开机检查同上下文,
             * 规避独立任务创建在部分启动场景不可靠的老坑; 阻塞主循环期间
             * LVGL 任务照常渲染, notify overlay 显示进度) */
            ESP_LOGI(TAG, "设置页手动触发 OTA 检查…");
            ota_client_request_check();
        } else if (s_page == PAGE_ABOUT && s_sel == 2) {
            ESP_LOGI(TAG, "设置页手动重启…");
            vTaskDelay(pdMS_TO_TICKS(200));   /* 串口日志 flush */
            esp_restart();
        }
        s_adjusting = false;
        refresh();
        nav_vibe();
        return;
    }

    int count;
    const item_t *items = page_items(s_page, &count);
    if (s_sel < 0 || s_sel >= count) return;
    const item_t *it = &items[s_sel];

    switch (it->type) {
    case IT_SUBPAGE:
        s_page = it->subpage;
        s_sel = first_selectable(s_page);
        refresh();
        nav_vibe();
        break;

    case IT_ADJUST:
        s_adjusting = true;
        refresh();
        nav_vibe();
        break;

    case IT_TOGGLE:
        if (s_page == PAGE_DISPLAY && s_sel == 2) {
            uint32_t en = config_get_u32(CFG_KEY_BAR_EN, 1) ? 0 : 1;
            config_set_u32(CFG_KEY_BAR_EN, en);
            brightness_bar_set_enabled(en != 0);
            refresh();
            nav_vibe();
        } else if (s_page == PAGE_TZ && s_sel == 0) {
            /* 自动时区: 开 → 应用缺省 +8 等下次拉取; 关 → 应用手动偏移 */
            uint32_t en = config_get_u32(CFG_KEY_TZ_AUTO, 1) ? 0 : 1;
            config_set_u32(CFG_KEY_TZ_AUTO, en);
            time_manager_apply_tz(en ? 28800 :
                (int32_t)config_get_u32(CFG_KEY_TZ_MAN, TZ_PRESET_DEF));
            refresh();
            nav_vibe();
        } else if (s_page == PAGE_STORAGE && s_sel == 0) {
            /* U盘模式: 切换挂载权 — /data 卸载/重挂, 状态即时刷新 */
            if (usb_storage_is_active())
                usb_storage_exit();
            else
                usb_storage_enter();
            refresh();
            nav_vibe();
        }
        break;

    case IT_INFO:
    default:
        break;
    }
}

/* ── BACK: 调值→列表, 子页→根页, 根页→退出设置 ── */
static void do_back(void)
{
    if (s_adjusting) {
        s_adjusting = false;
        refresh();
        nav_vibe();
        return;
    }
    if (s_page != PAGE_ROOT) {
        s_page = PAGE_ROOT;
        s_sel = first_selectable(PAGE_ROOT);
        refresh();
        nav_vibe();
        return;
    }
    /* 根页 BACK → 退出设置 */
    nav_vibe();
    if (s_close_cb) s_close_cb();
}

/* ══════ 对外接口 ══════ */

void settings_screen_input(settings_event_t ev)
{
    if (!s_active) return;
    switch (ev) {
    case SETTINGS_EV_UP:      /* 上一项 / 数值加 */
        if (s_adjusting) adjust_step(+1);
        else             move_sel(-1);
        break;
    case SETTINGS_EV_DOWN:    /* 下一项 / 数值减 */
        if (s_adjusting) adjust_step(-1);
        else             move_sel(+1);
        break;
    case SETTINGS_EV_CONFIRM: do_confirm(); break;
    case SETTINGS_EV_BACK:    do_back();    break;
    default: break;
    }
}

void settings_screen_set_close_cb(void (*cb)(void))
{
    s_close_cb = cb;
}

esp_err_t settings_screen_init(void)
{
    if (s_active) return ESP_OK;
    ESP_LOGI(TAG, "创建设置界面 v2 (子页面模型)…");

    s_page = PAGE_ROOT;
    s_sel = first_selectable(PAGE_ROOT);
    s_adjusting = false;
    s_last_vib = 0;
    cache_invalidate_all();     /* 首次 refresh 全量应用 */

    /* ── 屏幕: 纯黑; 关滚动条 (防内容溢出触发主题滚动条) ── */
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_scr, LV_SCROLLBAR_MODE_OFF);

    /* ── 顶栏 ── */
    lv_obj_t *hdr = lv_obj_create(s_scr);
    lv_obj_set_size(hdr, 240, 28);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    /* 左上角图标: ← (返回方向提示) + HOME/齿轮 (根页/子页, refresh 里切换)。
     * 符号字形在 montserrat 内, 中文字体不一定覆盖 → 与中文标题分开 */
    s_icons = lv_label_create(hdr);
    lv_label_set_text(s_icons, LV_SYMBOL_LEFT " " LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(s_icons, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(s_icons, &lv_font_montserrat_14, 0);
    lv_obj_align(s_icons, LV_ALIGN_LEFT_MID, 10, 0);

    s_title = lv_label_create(hdr);
    lv_label_set_text(s_title, s_page_title[s_page]);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xcccccc), 0);
    if (FONT_ZH) lv_obj_set_style_text_font(s_title, FONT_ZH, 0);
    else         lv_obj_set_style_text_font(s_title, &lv_font_montserrat_14, 0);
    lv_obj_align(s_title, LV_ALIGN_LEFT_MID, 48, 0);

    /* ── 预创建 5 行 ── */
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

        /* 父项指示箭头 (LV_SYMBOL_RIGHT, 白色) — 靠屏幕右缘,
         * 仅含子页面的项显示 (refresh 按类型刷新) */
        s_row_ind[i] = lv_label_create(row);
        lv_label_set_text(s_row_ind[i], "");
        lv_obj_set_style_text_color(s_row_ind[i], lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(s_row_ind[i], &lv_font_montserrat_14, 0);
        lv_obj_align(s_row_ind[i], LV_ALIGN_RIGHT_MID, -8, 0);

        s_row_lbl[i] = lv_label_create(row);
        lv_label_set_text(s_row_lbl[i], "");
        lv_obj_set_style_text_color(s_row_lbl[i], lv_color_hex(0xeeeeee), 0);
        if (FONT_ZH) lv_obj_set_style_text_font(s_row_lbl[i], FONT_ZH, 0);
        else         lv_obj_set_style_text_font(s_row_lbl[i], &lv_font_montserrat_14, 0);
        lv_obj_align(s_row_lbl[i], LV_ALIGN_LEFT_MID, 10, 0);

        /* 数值右对齐, 给右侧箭头留出位置 */
        s_row_val[i] = lv_label_create(row);
        lv_label_set_text(s_row_val[i], "");
        lv_obj_set_style_text_color(s_row_val[i], lv_color_hex(0x888888), 0);
        if (FONT_ZH) lv_obj_set_style_text_font(s_row_val[i], FONT_ZH, 0);
        else         lv_obj_set_style_text_font(s_row_val[i], &lv_font_montserrat_14, 0);
        lv_obj_align(s_row_val[i], LV_ALIGN_RIGHT_MID, -28, 0);

        /* 开关 (IT_TOGGLE 项专用, 默认隐藏)。真机无 LVGL indev,
         * 仅作状态显示; 切换仍走左键 CONFIRM。toggle 行不显示箭头。 */
        s_row_sw[i] = lv_switch_create(row);
        lv_obj_set_size(s_row_sw[i], 46, 24);
        lv_obj_align(s_row_sw[i], LV_ALIGN_RIGHT_MID, -10, 0);
        lv_obj_add_flag(s_row_sw[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_row_sw[i], lv_color_hex(0x2a2a33),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_row_sw[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_row_sw[i], lv_color_hex(0x4a90d9),
                                  LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(s_row_sw[i], lv_color_hex(0x4a90d9),
                                  LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(s_row_sw[i], LV_OPA_COVER,
                                LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(s_row_sw[i], lv_color_hex(0xdddddd),
                                  LV_PART_KNOB);

        s_rows[i] = row;
    }

    /* ── 内嵌提示框 (1.0.227): OTA 检查等设置页操作的结果提示。
     * 挂设置页 screen 顶层 (最后创建 = z 序最上), 离开设置页即不可见;
     * 居中偏上浮在列表上方, 短暂显示后自动消失 (auto_hide_ms) */
    s_toast = lv_obj_create(s_scr);
    lv_obj_set_size(s_toast, 216, 46);
    lv_obj_align(s_toast, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x1a1a22), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_toast, 10, 0);
    lv_obj_set_style_border_width(s_toast, 1, 0);
    lv_obj_set_style_border_color(s_toast, lv_color_hex(0x333344), 0);
    lv_obj_set_style_pad_all(s_toast, 0, 0);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    s_toast_lbl = lv_label_create(s_toast);
    lv_label_set_long_mode(s_toast_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_toast_lbl, 200);
    lv_obj_center(s_toast_lbl);
    lv_obj_set_style_text_color(s_toast_lbl, lv_color_hex(0x88CCFF), 0);
    lv_obj_set_style_text_font(s_toast_lbl, FONT_ZH, 0);
    lv_obj_set_style_text_align(s_toast_lbl, LV_TEXT_ALIGN_CENTER, 0);
    s_toast_timer = lv_timer_create(toast_poll_cb, 100, NULL);
    lv_timer_set_repeat_count(s_toast_timer, -1);

    refresh();

    s_active = true;
    lv_scr_load(s_scr);
    ESP_LOGI(TAG, "设置界面已显示");
    return ESP_OK;
}

void settings_screen_destroy(void)
{
    if (!s_active) return;
    /* 定时器不随对象删除 — 必须显式清理, 否则悬垂回调 (toast_poll_cb
     * 引用已删的 s_toast) 崩溃 */
    if (s_toast_hide)  { lv_timer_delete(s_toast_hide);  s_toast_hide  = NULL; }
    if (s_toast_timer) { lv_timer_delete(s_toast_timer); s_toast_timer = NULL; }
    if (s_scr) {
        lv_obj_del(s_scr);
        s_scr = NULL;
    }
    s_icons = NULL;
    s_title = NULL;
    for (int i = 0; i < MAX_VISIBLE; i++) {
        s_rows[i] = NULL;
        s_row_ind[i] = NULL;
        s_row_lbl[i] = NULL;
        s_row_val[i] = NULL;
        s_row_sw[i] = NULL;
    }
    s_active = false;
    s_adjusting = false;
    s_toast = NULL;   /* 子树已随 s_scr 删除, 仅清指针 */
    s_toast_lbl = NULL;
    ESP_LOGI(TAG, "设置界面已关闭");
}

bool settings_screen_is_active(void)
{
    return s_active;
}

/* ── 设置页内嵌提示框 ── */

static void toast_hide_cb(lv_timer_t *t)
{
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    s_toast_hide = NULL;
}

static void toast_poll_cb(lv_timer_t *t)
{
    if (!s_toast_req) return;
    s_toast_req = false;

    lv_label_set_text(s_toast_lbl, s_toast_text);
    lv_color_t tc = lv_color_hex(0x88CCFF);   /* info: 浅蓝 */
    if (s_toast_type == NOTIFY_WARN)  tc = lv_color_hex(0xFFCC44);
    if (s_toast_type == NOTIFY_ERROR) tc = lv_color_hex(0xFF4444);
    lv_obj_set_style_text_color(s_toast_lbl, tc, 0);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    /* 删除后必须置空 — 曾漏置空导致二次删除已释放定时器
     * (notify_overlay 同款教训) */
    if (s_toast_hide) { lv_timer_delete(s_toast_hide); s_toast_hide = NULL; }
    if (s_toast_auto_ms > 0) {
        s_toast_hide = lv_timer_create(toast_hide_cb, s_toast_auto_ms, NULL);
        lv_timer_set_repeat_count(s_toast_hide, 1);
    }
}

void settings_screen_notify(notify_type_t type, const char *text, uint32_t auto_hide_ms)
{
    if (!text || !text[0]) return;
    s_toast_type = type;
    strncpy(s_toast_text, text, sizeof(s_toast_text) - 1);
    s_toast_text[sizeof(s_toast_text) - 1] = '\0';
    s_toast_auto_ms = auto_hide_ms;
    s_toast_req = true;
}
