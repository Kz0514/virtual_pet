/**
 * @file hw_wifi.h
 * @brief I 段 WiFi 射频通路 (通讯) —— 扫到 AP 即证明 RF 收发通
 *
 * 凭据纪律: SSID/密码从 main/hw_secrets.h 读 (gitignored, 由 run.py --wifi-connect
 * 生成), 绝不走命令行参数。没有凭据时只扫描不连接 ("wifi.conn" = SKIP)。
 * 测完 esp_wifi_deinit + 销毁 netif —— 把内部 RAM 让给末尾的屏幕看板。
 */
#ifndef HW_WIFI_H
#define HW_WIFI_H

/* I 段: MAC / PHY 校准数据 / 主动扫描 / (可选)连接拿 IP */
void hw_wifi_run(void);

#endif /* HW_WIFI_H */
