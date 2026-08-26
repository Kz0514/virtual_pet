/**
 * @file wifi_scanner.c
 * @brief WiFi AP scanning implementation
 */
#include "wifi_scanner.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "wifi_scan";

int wifi_scan_aps(wifi_ap_info_t *out)
{
    if (!out) return 0;

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);  /* blocking */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan start failed: %d", err);
        return 0;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        ESP_LOGI(TAG, "No APs found");
        return 0;
    }

    if (ap_count > WIFI_SCAN_MAX_APS) ap_count = WIFI_SCAN_MAX_APS;

    wifi_ap_record_t *records = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!records) return 0;

    esp_wifi_scan_get_ap_records(&ap_count, records);

    for (int i = 0; i < ap_count; i++) {
        snprintf(out[i].mac, sizeof(out[i].mac),
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 records[i].bssid[0], records[i].bssid[1],
                 records[i].bssid[2], records[i].bssid[3],
                 records[i].bssid[4], records[i].bssid[5]);
        out[i].rssi = records[i].rssi;
    }

    free(records);
    ESP_LOGI(TAG, "Scanned %d APs", ap_count);
    return ap_count;
}


int wifi_scan_build_json(const wifi_ap_info_t *aps, int count,
                         char *buf, int buf_len)
{
    if (!aps || !buf || count <= 0) return 0;

    int pos = snprintf(buf, buf_len, "[");
    if (pos < 0 || pos >= buf_len) return 0;

    for (int i = 0; i < count; i++) {
        int n = snprintf(buf + pos, buf_len - pos,
                         "%s{\"mac\":\"%s\",\"rssi\":%d}",
                         (i > 0) ? "," : "", aps[i].mac, aps[i].rssi);
        if (n < 0 || pos + n >= buf_len) {
            buf[pos] = '\0';
            return pos;
        }
        pos += n;
    }

    int n = snprintf(buf + pos, buf_len - pos, "]");
    if (n > 0) pos += n;
    return pos;
}
