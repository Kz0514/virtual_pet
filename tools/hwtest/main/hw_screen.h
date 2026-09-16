/**
 * @file hw_screen.h
 * @brief 屏幕看板 (不引 LVGL): 56 个格子 = 56 个测试项, 绿/红/灰/橙 = PASS/FAIL/SKIP/WARN
 *
 * 这是**旁证**, 不是判据 —— 判据在串口 JSON (run.py 解析)。字模只有数字 + 少数大写字母
 * (够写 PASS/FAIL/SKIP/HWTEST/编号), 细节一律看串口。
 * 必须最后调用: 115KB 帧缓冲先要内部 RAM, 拿不到才退 PSRAM。
 */
#ifndef HW_SCREEN_H
#define HW_SCREEN_H

/* 画看板 + 打开背光。返回 ESP_OK 表示帧缓冲分配并推屏成功 */
int hw_screen_dashboard(void);

#endif /* HW_SCREEN_H */
