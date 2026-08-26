/**
 * @file wifi_scanner.h
 * @brief WiFi AP scanning for Tencent network location API
 *
 * Scans surrounding WiFi access points and returns MAC + RSSI pairs
 * for the server-side Tencent Map network positioning API.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum APs to scan */
#define WIFI_SCAN_MAX_APS   15

/** A single WiFi AP fingerprint */
typedef struct {
    char    mac[18];   /**< "aa:bb:cc:dd:ee:ff" format */
    int8_t  rssi;      /**< signal strength in dBm */
} wifi_ap_info_t;

/**
 * @brief Scan surrounding WiFi APs.
 * @param out   Output buffer (caller-allocated, WIFI_SCAN_MAX_APS entries)
 * @return      Number of APs found, 0 on failure
 */
int wifi_scan_aps(wifi_ap_info_t *out);

/**
 * @brief Build a JSON array of scanned APs for the Tencent API.
 * @param aps        Scanned AP list
 * @param count      Number of APs
 * @param buf        Output JSON buffer (caller-allocated)
 * @param buf_len    Buffer size
 * @return           Number of bytes written (excluding null terminator), or 0 on overflow
 */
int wifi_scan_build_json(const wifi_ap_info_t *aps, int count,
                         char *buf, int buf_len);

#ifdef __cplusplus
}
#endif
