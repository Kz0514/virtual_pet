/**
 * @file config_keys.h
 * @brief NVS 配置键名唯一权威定义 — 所有 config_get/set_* 调用方统一使用
 *
 * 红线: 键名字符串一旦发布不得改动 — NVS 存量数据按键名匹配
 * (firmware 已上传的设备, 改名即丢用户配置)。NVS 键名上限 15 字节。
 */
#pragma once

#define CFG_KEY_PET_NAME     "pet_name"      /* 宠物名 */
#define CFG_KEY_OWNER_NAME   "owner_name"    /* 主人称谓 */
#define CFG_KEY_BRI          "bri"           /* 用户亮度 1-100 */
#define CFG_KEY_BAR_EN       "bri_bar_en"    /* 主界面亮度条开关 */
#define CFG_KEY_OFF_S        "off_s"         /* 自动息屏秒数 (设置页可调) */
#define CFG_KEY_NAV_MODE     "nav_mode"      /* 菜单导航: 0=点击 1=滑动 */
#define CFG_KEY_SCROLL_FLIP  "scroll_flip"   /* 日记翻页方向: 1=反转 */
#define CFG_KEY_TZ_AUTO      "tz_auto"       /* 时区: 1=自动(IP) 0=手动 */
#define CFG_KEY_TZ_MANUAL    "tz_manual_sec" /* 手动时区偏移秒 */
#define CFG_KEY_TZ_LAST      "tz_last"       /* 上次成功拉取 unix 秒 */
#define CFG_KEY_DIARY_DAY    "diary_day"     /* 日记今日 YYYYMMDD */
#define CFG_KEY_DIARY_CNT    "diary_cnt"     /* 日记今日互动计数 */