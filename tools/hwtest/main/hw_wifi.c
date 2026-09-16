/**
 * @file hw_wifi.c
 * @brief I 段 —— esp_wifi STA 起机 → MAC/PHY → 主动扫描 → (可选)连一次拿 IP
 *
 * PHY 校准数据的读法照抄 main/system/boot_init.c:291-309 (nvs "phy" 命名空间)。
 */
#include "hw_wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hw_expect.h"
#include "hw_report.h"
#include "hw_secrets.h"
#include "nvs.h"

static const char *TAG = "hwtest";

static esp_netif_t *s_netif = NULL;

/* ══════════════════════════════════════════════════════════════════════
 * ① 身份: MAC 与 efuse 出厂值比对
 * ══════════════════════════════════════════════════════════════════════ */
static void test_mac(void)
{
    hw_item_t *it = hw_begin("wifi.mac", "WiFi MAC");
    uint8_t mac[6] = {0}, efuse[6] = {0};
    esp_err_t e1 = esp_wifi_get_mac(WIFI_IF_STA, mac);
    esp_err_t e2 = esp_read_mac(efuse, ESP_MAC_WIFI_STA);
    char ms[20];
    snprintf(ms, sizeof(ms), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
    hw_set(it, "%s", ms);
    bool all0 = true, allf = true;
    for (int i = 0; i < 6; i++) {
        if (mac[i]) all0 = false;
        if (mac[i] != 0xFF) allf = false;
    }
    if (e1 != ESP_OK || e2 != ESP_OK) {
        hw_note(it, "读 MAC 失败 (wifi=%s efuse=%s)", esp_err_to_name(e1), esp_err_to_name(e2));
        hw_end(it, HW_ST_FAIL);
    } else if (all0 || allf) {
        hw_note(it, "MAC 全 %s → efuse 未烧", all0 ? "0" : "FF");
        hw_end(it, HW_ST_FAIL);
    } else if (memcmp(mac, efuse, 6) != 0) {
        /* 曾被 set_mac 改过也不算坏 → 只提示 */
        hw_note(it, "与 efuse 出厂值 %02X:%02X:%02X:%02X:%02X:%02X 不同 (可能被改过)",
                efuse[0], efuse[1], efuse[2], efuse[3], efuse[4], efuse[5]);
        hw_end(it, HW_ST_WARN);
    } else {
        hw_end(it, HW_ST_PASS);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * ② PHY 校准数据 (NVS "phy": cal_version / cal_data)
 * ══════════════════════════════════════════════════════════════════════ */
static void test_phy(void)
{
    hw_item_t *it = hw_begin("wifi.phy", "PHY 校准数据");
    nvs_handle_t h;
    esp_err_t e = nvs_open("phy", NVS_READONLY, &h);
    if (e != ESP_OK) {
        hw_set(it, "无 phy 命名空间");
        /* 全新板 / 从未连过网: 首次 RF 初始化后才会落盘 → 不是故障 */
        hw_note(it, "校准数据从未保存 (新板正常, 跑一次扫描后应出现)");
        hw_end(it, HW_ST_WARN);
        return;
    }
    uint32_t ver = 0;
    size_t len = 0;
    nvs_get_u32(h, "cal_version", &ver);
    esp_err_t be = nvs_get_blob(h, "cal_data", NULL, &len);
    nvs_close(h);
    hw_set(it, "cal_version=%lu cal_data=%uB", (unsigned long)ver, (unsigned)len);
    if (be != ESP_OK || len == 0) {
        hw_note(it, "有 phy 命名空间但没有 cal_data");
        hw_end(it, HW_ST_WARN);
    } else if (len < 64 || len > 4096) {
        hw_note(it, "cal_data 长度异常");
        hw_end(it, HW_ST_FAIL);
    } else {
        hw_end(it, HW_ST_PASS);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * ③ 主动扫描: 扫到 ≥1 个 AP = 射频收发链路通
 * ══════════════════════════════════════════════════════════════════════ */
static void test_scan(void)
{
    hw_item_t *it = hw_begin("wifi.scan", "WiFi 主动扫描");
    wifi_scan_config_t sc = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0, /* 全信道 */
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    int64_t t0 = esp_timer_get_time();
    esp_err_t e = esp_wifi_scan_start(&sc, true); /* 阻塞 */
    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (e != ESP_OK) {
        hw_set(it, "scan_start=%s", esp_err_to_name(e));
        hw_note(it, "扫描没起来 (%dms) — RF 前端/天线通路要看", ms);
        hw_end(it, HW_ST_FAIL);
        return;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    wifi_ap_record_t *recs = NULL;
    uint16_t cap = n > 12 ? 12 : n; /* 只要最强的几个, 省内存 */
    if (cap) {
        recs = malloc(sizeof(wifi_ap_record_t) * cap);
        if (recs) esp_wifi_scan_get_ap_records(&cap, recs);
    } else {
        esp_wifi_clear_ap_list();
    }

    /* 取 RSSI 最强 3 个 */
    int best[3] = {-1, -1, -1};
    for (int i = 0; i < (int)cap && recs; i++) {
        for (int k = 0; k < 3; k++) {
            if (best[k] < 0 || recs[i].rssi > recs[best[k]].rssi) {
                for (int j = 2; j > k; j--) best[j] = best[j - 1];
                best[k] = i;
                break;
            }
        }
    }
    char top[80];
    top[0] = '\0';
    size_t u = 0;
    for (int k = 0; k < 3 && best[k] >= 0; k++) {
        u += (size_t)snprintf(top + u, sizeof(top) - u, "%s\"%s\" %ddBm ch%u",
                              u ? " " : "", (const char *)recs[best[k]].ssid, recs[best[k]].rssi,
                              recs[best[k]].primary);
    }
    if (recs) free(recs);

    hw_set(it, "%u 个 AP (%dms)", n, ms);
    if (n == 0) {
#if HWTEST_ALLOW_NO_WIFI
        hw_note(it, "0 个 AP — 已按 --allow-no-wifi 降级 (屏蔽房/暗室场景)");
        hw_end(it, HW_ST_WARN);
#else
        hw_note(it, "0 个 AP → RF 收发链路可疑 (屏蔽环境请用 --allow-no-wifi)");
        hw_end(it, HW_ST_FAIL);
#endif
    } else {
        hw_note(it, "%s", top);
        hw_end(it, HW_ST_PASS);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * ④ 可选: 连一次拿 IP (最强判据 —— "WiFi 真能用")
 * ══════════════════════════════════════════════════════════════════════ */
static void test_connect(void)
{
    hw_item_t *it = hw_begin("wifi.conn", "WiFi 连接 + DHCP");
    const char *ssid = HWTEST_WIFI_SSID;
    if (!ssid[0]) {
        hw_set(it, "未提供凭据");
        hw_note(it, "用 run.py --wifi-connect SSID:密码 打开 (默认只扫描)");
        hw_end(it, HW_ST_SKIP);
        return;
    }
    wifi_config_t wc = {0};
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", ssid);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", HWTEST_WIFI_PASS);
    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (e == ESP_OK) e = esp_wifi_connect();
    if (e != ESP_OK) {
        hw_set(it, "连接请求失败: %s", esp_err_to_name(e));
        hw_end(it, HW_ST_FAIL);
        return;
    }

    esp_netif_ip_info_t ip = {0};
    bool got = false;
    int waited = 0;
    while (waited < HW_EXP_WIFI_CONNECT_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(250));
        waited += 250;
        if (s_netif && esp_netif_get_ip_info(s_netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            got = true;
            break;
        }
    }
    esp_wifi_disconnect();
    if (!got) {
        hw_set(it, "\"%s\" %dms 内未拿到 IP", ssid, waited);
        hw_note(it, "密码错/信道太挤/AP 不可达");
        hw_end(it, HW_ST_FAIL);
    } else {
        hw_set(it, "\"%s\" IP " IPSTR " 网关 " IPSTR " (%dms)", ssid, IP2STR(&ip.ip),
               IP2STR(&ip.gw), waited);
        hw_end(it, HW_ST_PASS);
    }
}

void hw_wifi_run(void)
{
    ESP_LOGI(TAG, "──── I 段: WiFi ────");

#if HWTEST_SKIP_WIFI
    hw_skip("wifi.skip", "WiFi 段", "命令行 --skip-wifi");
    return;
#else
    hw_item_t *it = hw_begin("wifi.init", "WiFi 初始化");
    esp_err_t e1 = esp_netif_init();
    esp_err_t e2 = esp_event_loop_create_default();
    s_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t e3 = esp_wifi_init(&cfg);
    esp_err_t e4 = esp_wifi_set_mode(WIFI_MODE_STA);
    esp_err_t e5 = esp_wifi_start();
    hw_set(it, "netif=%s event=%s wifi=%s mode=%s start=%s", esp_err_to_name(e1),
           esp_err_to_name(e2), esp_err_to_name(e3), esp_err_to_name(e4),
           esp_err_to_name(e5));
    if (e3 != ESP_OK || e4 != ESP_OK || e5 != ESP_OK || !s_netif) {
        hw_note(it, "WiFi 栈没起来 — 后面几项都会跟着挂");
        hw_end(it, HW_ST_FAIL);
        goto teardown;
    }
    hw_end(it, HW_ST_PASS);

    test_mac();
    test_phy();
    test_scan();
    test_connect();

teardown:
    /* 释放 RF/网络栈: 末尾看板要 115KB 连续内部 RAM */
    if (s_netif) {
        esp_wifi_stop();
        esp_wifi_deinit();
        esp_netif_destroy_default_wifi(s_netif);
        s_netif = NULL;
    }
    ESP_LOGI(TAG, "WiFi 栈已拆");
#endif
}
