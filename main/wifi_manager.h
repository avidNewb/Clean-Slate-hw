#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool open;
} wifi_scan_entry_t;

/**
 * Initialize Wi-Fi using saved NVS settings. Starts Captive Portal SoftAP mode
 * automatically if no Wi-Fi credentials are configured yet.
 */
esp_err_t wifi_manager_init(void);

/**
 * Block until Wi-Fi STA obtains an IPv4 address or timeout expires.
 */
bool wifi_manager_wait_connected(uint32_t timeout_ms);

/**
 * Return true if STA is currently connected with a valid IPv4 address.
 */
bool wifi_manager_is_connected(void);

/**
 * Return true if the SoftAP setup portal is currently active.
 */
bool wifi_manager_is_ap_mode(void);

/**
 * Switch to (or stay in) Captive Portal AP+STA setup mode with DNS redirect.
 */
esp_err_t wifi_manager_enter_setup_mode(void);

/**
 * Apply new STA credentials, leave setup mode when connected (or immediately if exit_ap),
 * and connect with exponential backoff.
 */
esp_err_t wifi_manager_apply_sta_credentials(const char *ssid, const char *password, bool exit_ap);

/**
 * Perform a blocking Wi-Fi scan and return up to max_entries deduplicated networks sorted by RSSI.
 */
esp_err_t wifi_manager_scan_networks(wifi_scan_entry_t *out_entries,
                                     size_t max_entries,
                                     size_t *out_count);

/**
 * Copy current STA IPv4 address string (e.g. "192.168.1.42") into buf.
 */
void wifi_manager_get_ip_str(char *buf, size_t buf_len);

/**
 * Copy primary STA DNS IPv4 address string (e.g. "192.168.1.1") into buf.
 */
void wifi_manager_get_dns_str(char *buf, size_t buf_len);

/**
 * Get device ID / mDNS-style hostname string (e.g. "clean-slate-a1b2c3").
 */
const char *wifi_manager_get_device_id(void);

/**
 * Get Captive Portal SoftAP SSID (e.g. "CleanSlate-B2C3").
 */
const char *wifi_manager_get_ap_ssid(void);

/**
 * Get Captive Portal SoftAP WPA2 passphrase (e.g. "CS-A1B2C3").
 */
const char *wifi_manager_get_ap_password(void);

/**
 * Get currently configured STA SSID.
 */
void wifi_manager_get_sta_ssid(char *buf, size_t buf_len);

/**
 * Get current STA RSSI in dBm, or 0 if disconnected.
 */
int wifi_manager_get_rssi(void);

#ifdef __cplusplus
}
#endif
