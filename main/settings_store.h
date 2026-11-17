#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char api_base_url[192];
    char device_name[48];
    char favorite_teams[96]; /* Comma-separated uppercase abbreviations, e.g. "ALA" or "ALA,TEX" */
    char timezone[48];
    bool quiet_enabled;
    char quiet_start[8];
    char quiet_end[8];
    int brightness_override; /* -1 = Auto light sensor, 0..255 = manual */
    uint8_t brightness_min;  /* 1..255 auto-brightness floor (default 15) */
    uint8_t brightness_max;  /* 1..255 auto-brightness ceiling (default 255) */
    uint8_t df_volume;       /* 0..30 master DFPlayer volume (0 = mute) */
    uint8_t df_eq;           /* 0..5 DFPlayer EQ (0=Normal..5=Bass) */
    bool boot_tone;
    bool hourly_chime;
    uint16_t rotary_idle_sec;  /* 0 = stay pinned, >0 = return to Auto after idle */
    uint16_t poll_interval_sec;/* Fallback API poll interval (1..3600) */
    bool ota_enabled;          /* Enable background OTA check */
    char ntp_server[64];       /* SNTP server hostname */
    uint8_t fight_folder;      /* 0 = global DFPlayer track index, 1..99 = folder */
    uint16_t fight_track;      /* Default 1 */
    uint8_t victory_folder;    /* 0 = global DFPlayer track index, 1..99 = folder */
    uint16_t victory_track;    /* Default 2 */
    uint8_t third_folder;      /* 0 = global DFPlayer track index, 1..99 = folder */
    uint16_t third_track;      /* Default 3 */
    char pinned_app[32];       /* Empty = Auto schedule, else pinned app_id */
    bool cloud_sync_pending;
} cs_local_settings_t;

/**
 * Erase all NVS configuration keys and restore defaults (factory reset).
 */
esp_err_t settings_store_factory_reset(void);

/**
 * Initialize NVS-backed settings store and load persisted values (with Kconfig defaults).
 */
esp_err_t settings_store_init(void);

/**
 * Copy current thread-safe snapshot of settings into out.
 */
void settings_store_get(cs_local_settings_t *out);

/**
 * Replace settings in memory and persist to NVS.
 */
esp_err_t settings_store_save(const cs_local_settings_t *in);

/**
 * Clear stored Wi-Fi credentials in NVS (forces captive portal setup mode on boot/reset).
 */
esp_err_t settings_store_clear_wifi(void);

/**
 * Return true if non-empty Wi-Fi credentials are configured.
 */
bool settings_store_has_wifi(void);

/**
 * Copy current API base URL (guaranteed non-empty).
 */
void settings_store_get_api_base_url(char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif
