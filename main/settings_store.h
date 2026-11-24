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
    char pinned_app[64];       /* Empty = Auto schedule, else pinned app_id or app_id:screen_id */
    char device_token[72];     /* Per-device bearer token enrolled from cloud server (NVS key "dev_tok") */
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
 * Copy current enrolled device bearer token into out (empty if not yet enrolled).
 */
void settings_store_get_device_token(char *out, size_t out_cap);

/**
 * Update and persist only the enrolled device bearer token (empty string clears it).
 */
esp_err_t settings_store_set_device_token(const char *token);

#define CS_MAX_LOCAL_ALARMS 8

#define CS_ALARM_POST_NONE 0U
#define CS_ALARM_POST_BIBLE_VOTD 1U
#define CS_ALARM_POST_BIBLE_VOTD_GENTLE 2U

typedef struct {
    bool enabled;
    bool repeat;         /* false = one-time alarm (auto-disables after ringing); true = repeat on days_mask */
    uint8_t hour;        /* 0..23 */
    uint8_t minute;      /* 0..59 */
    uint8_t days_mask;   /* bit 0=Sun .. 6=Sat (matches tm_wday); only used when repeat==true */
    uint8_t folder;      /* DFPlayer folder (1..99) */
    uint8_t track;       /* DFPlayer track (1..255) */
    uint8_t theme_id;    /* 0=sunrise, 1=digital_pulse, 2=siren_strobe, 3=gentle_chime */
    char id[25];         /* Alarm ID matching cloud config (e.g. "alarm_1") */
    char label[25];      /* Short uppercase label, e.g. "WAKE UP!" */
    uint8_t post_action; /* CS_ALARM_POST_* */
    uint8_t post_minutes;/* 1..60, default 5 */
} cs_alarm_item_t;

typedef struct {
    bool app_enabled;
    bool allow_during_quiet_hours;
    bool use_24h;
    uint8_t duration_minutes; /* 1..15, default 2 */
    uint8_t volume;           /* 0..30, default 26 */
    uint8_t count;            /* 0..CS_MAX_LOCAL_ALARMS */
    cs_alarm_item_t items[CS_MAX_LOCAL_ALARMS];
    uint8_t pending_delete_count;
    char pending_delete_ids[CS_MAX_LOCAL_ALARMS][25];
} cs_alarm_schedule_t;

/**
 * Copy current thread-safe snapshot of locally cached alarm schedule into out.
 */
void settings_store_get_alarms(cs_alarm_schedule_t *out);

/**
 * Save locally cached alarm schedule in memory and persist to NVS when changed.
 */
esp_err_t settings_store_save_alarms(const cs_alarm_schedule_t *in);

/**
 * Delete a fired one-time alarm locally from NVS and queue its ID for cloud DELETE sync.
 */
esp_err_t settings_store_consume_onetime_alarm(const char *alarm_id);

/**
 * Remove an alarm ID from the pending cloud-sync queue after cloud sync succeeds.
 */
esp_err_t settings_store_clear_pending_alarm_delete(const char *alarm_id);

/**
 * Clear all pending cloud alarm-disable sync entries (called when user explicitly saves alarms).
 */
esp_err_t settings_store_clear_all_pending_alarm_deletes(void);

/**
 * Parse a JSON object for `app_settings.alarm` (supporting both multi-alarm `alarms`
 * array and legacy single-alarm keys) into a normalized cs_alarm_schedule_t.
 */
void settings_store_parse_alarms_json(bool app_enabled,
                                      const char *alarm_obj_json,
                                      size_t alarm_obj_len,
                                      cs_alarm_schedule_t *out);

/**
 * Return the track-length-aligned repeat interval (ms) for a DFPlayer (folder, track) cue.
 */
uint32_t settings_store_alarm_repeat_interval_ms(uint8_t folder, uint8_t track);

/**
 * Copy current API base URL (guaranteed non-empty).
 */
void settings_store_get_api_base_url(char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif
