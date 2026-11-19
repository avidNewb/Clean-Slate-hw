#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "gfx_font.h"
#include "settings_store.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CS_FRAME_WIDTH          64
#define CS_FRAME_HEIGHT         32
#define CS_FRAME_PIXELS         (CS_FRAME_WIDTH * CS_FRAME_HEIGHT)
#define CS_FRAME_RGB565_BYTES   (CS_FRAME_PIXELS * 2)
#define CS_FRAME_RGB888_BYTES   (CS_FRAME_PIXELS * 3)

#define CS_MAX_APPS             16
#define CS_MAX_SCREENS_PER_APP  8
#define CS_MAX_ROTARY_SLOTS     24
#define CS_MAX_OVERLAYS         8
#define CS_MAX_ALERT_FRAMES     32

typedef enum {
    CS_OVERLAY_NONE = 0,
    CS_OVERLAY_NTP_TIME,
    CS_OVERLAY_NTP_DATE,
    CS_OVERLAY_COUNTDOWN,
    CS_OVERLAY_TEXT,
} cs_overlay_type_t;

typedef struct {
    cs_overlay_type_t type;
    char id[32];
    int16_t x;
    int16_t y;
    gfx_font_id_t font;
    gfx_align_t align;
    uint16_t color_rgb565;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    char format[32];
    char text[64];
    int64_t target_epoch;
} cs_overlay_t;

typedef struct {
    char screen_id[24];
    char name[32];
} cs_screen_desc_t;

typedef struct {
    char app_id[32];
    cs_screen_desc_t screens[CS_MAX_SCREENS_PER_APP];
    size_t screen_count;
} cs_app_screens_t;

typedef struct {
    char key[64];      /* "app_id:screen_id" or "app_id" */
    char app_id[32];
    char screen_id[24];
    char label[32];
} cs_rotary_slot_t;

typedef struct {
    bool valid;
    char device_name[48];
    char favorite_teams[96];
    bool enabled_apps_present;
    char enabled_apps[CS_MAX_APPS][32];
    size_t enabled_app_count;
    bool active_events_known;
    bool active_event_birthdays;
    bool active_event_holidays;
    bool quiet_enabled;
    char quiet_start[8];
    char quiet_end[8];
    char timezone[48];
    char posix_tz[64];
    int brightness_override; /* -1 when null/unset, else 0..255 */
} cs_device_config_t;

typedef struct {
    bool valid;
    char event_type[32];
    uint16_t frame_count;
    uint16_t frame_duration_ms;
    uint8_t *strip_data;
    size_t strip_data_len;
    bool strip_is_zlib;
    char marquee_text[160];
    int16_t marquee_y;
    uint8_t marquee_r;
    uint8_t marquee_g;
    uint8_t marquee_b;
    bool has_audio;
    uint8_t audio_folder;
    uint8_t audio_track;
    uint8_t audio_volume;
    bool audio_repeat;
    uint32_t audio_repeat_interval_ms;
    uint32_t audio_max_duration_s;
} cs_alert_t;

typedef struct {
    bool not_modified;
    char etag[48];
    char app_id[32];
    char screen_id[24];
    uint32_t refresh_seconds;
    bool quiet_mode;
    uint8_t bg_rgb565be[CS_FRAME_RGB565_BYTES];
    uint8_t bg_rgb888[CS_FRAME_RGB888_BYTES];
    cs_overlay_t overlays[CS_MAX_OVERLAYS];
    size_t overlay_count;
    cs_alert_t alert;
} cs_frame_response_t;

typedef struct cs_strip_stream cs_strip_stream_t;

/**
 * Fetch /api/v1/device/{device_id}/config and populate out_cfg.
 */
esp_err_t api_client_fetch_config(cs_device_config_t *out_cfg);

typedef struct {
    const char *enabled_apps_json;   /* Raw JSON array e.g. "[]" or "[\"clock\"]", NULL = omit */
    const char *selection_mode;      /* "priority" or "rotate", NULL/empty = omit */
    int rotation_seconds;            /* 5..300 when >= 5, <= 0 = omit */
    int rotate_include_clock;        /* 0 = false, 1 = true, < 0 = omit */
    const char *app_settings_json;   /* Raw JSON object "{...}", NULL = omit */
    const char *app_schedules_json;  /* Raw JSON object "{...}", NULL = omit */
} cs_cloud_config_extras_t;

/**
 * Push local device configuration and optional multi-app extras
 * (enabled_apps, selection_mode, rotation_seconds, rotate_include_clock,
 * app_settings, app_schedules) to POST /api/v1/device/{device_id}/config.
 */
esp_err_t api_client_post_config_ext(const cs_local_settings_t *settings,
                                     const cs_cloud_config_extras_t *extras);

/**
 * Push local device configuration (favorite teams, name, quiet hours, brightness,
 * optional app_settings raw JSON object) to POST /api/v1/device/{device_id}/config.
 */
esp_err_t api_client_post_config(const cs_local_settings_t *settings,
                                 const char *enabled_apps_csv,
                                 const char *app_settings_json_obj);

/**
 * Fetch raw JSON from GET /api/v1/apps for local web UI dynamic schema rendering.
 * Caller must free(*out_json).
 */
esp_err_t api_client_fetch_apps_json(char **out_json, size_t *out_len);

/**
 * Return RAM-cached /api/v1/apps JSON if available (unless force_refresh is true),
 * otherwise fetch from cloud and cache. Caller must free(*out_json).
 */
esp_err_t api_client_get_cached_apps_json(bool force_refresh, char **out_json, size_t *out_len);

/**
 * Fetch raw JSON from GET /api/v1/device/{device_id}/config.
 * Caller must free(*out_json).
 */
esp_err_t api_client_fetch_device_config_json(char **out_json, size_t *out_len);

/**
 * Return RAM-cached /api/v1/device/{device_id}/config JSON if available (unless force_refresh
 * is true), otherwise fetch from cloud and cache. Caller must free(*out_json).
 */
esp_err_t api_client_get_cached_device_config_json(bool force_refresh, char **out_json, size_t *out_len);

/**
 * Delete a consumed one-time alarm on the cloud server via DELETE /api/v1/device/{id}/alarms/{alarm_id}.
 */
esp_err_t api_client_delete_alarm(const char *alarm_id);

/**
 * Fetch /api/v1/apps metadata and cache app screens[] descriptors internally.
 */
esp_err_t api_client_sync_apps_metadata(void);

/**
 * Fetch active_events {birthdays, holidays} from /api/v1/device/{device_id}/diagnostics.
 */
esp_err_t api_client_fetch_active_events(bool *birthdays_active, bool *holidays_active);

/**
 * Expand enabled_apps from cfg into (app_id, screen_id) rotary slots using cached
 * app.screens[] metadata, skipping idle event apps (birthdays/holidays) when known inactive.
 */
size_t api_client_expand_rotary_slots(const cs_device_config_t *cfg,
                                      cs_rotary_slot_t *out_slots,
                                      size_t max_slots);

/**
 * Fetch /api/v1/device/{device_id}/frame[?app_id=...&screen=...&teams=...&consume_alerts=0&compress=1]
 * using a persistent HTTP/TLS keep-alive session and optional If-None-Match ETag.
 * On HTTP 304 Not Modified, sets out_frame->not_modified=true and returns ESP_OK.
 * Pass consume_alerts=false for background cache pre-warming so one-time alerts are not stolen.
 * Caller must invoke api_client_free_alert(&out_frame->alert) after use.
 */
esp_err_t api_client_fetch_frame_ex(const char *app_id_override,
                                    const char *screen_override,
                                    const char *teams_override,
                                    bool consume_alerts,
                                    const char *if_none_match,
                                    cs_frame_response_t *out_frame);

/**
 * Convenience wrapper around api_client_fetch_frame_ex with consume_alerts=true.
 */
esp_err_t api_client_fetch_frame(const char *app_id_override,
                                 const char *screen_override,
                                 const char *teams_override,
                                 cs_frame_response_t *out_frame);

/**
 * Release dynamically allocated alert strip resources.
 */
void api_client_free_alert(cs_alert_t *alert);

/**
 * Begin streaming 4,096-byte RGB565BE frames from a decoded alert strip.
 */
cs_strip_stream_t *api_client_strip_stream_begin(const cs_alert_t *alert);

/**
 * Read next 4,096-byte RGB565BE frame from stream. Returns false on end/error.
 */
bool api_client_strip_stream_next(cs_strip_stream_t *stream, uint8_t *out_rgb565be_4096);

/**
 * Free strip stream resources (including ESP32 ROM tinfl state and dictionary).
 */
void api_client_strip_stream_end(cs_strip_stream_t *stream);

/**
 * Query /api/v1/firmware/check?current_version=... and perform esp_https_ota if
 * update_available is true. Returns ESP_OK if no update (reboots on OTA success).
 * Optionally populates update_avail_out and detail_out (latest version or reason).
 */
esp_err_t api_client_check_and_run_ota(const char *current_version,
                                       bool *update_avail_out,
                                       char *detail_out,
                                       size_t detail_cap);

/**
 * Return true once after an OTA-initiated esp_restart() so startup boot tones can
 * be suppressed following background firmware updates.
 */
bool api_client_consume_ota_reboot_flag(void);

typedef struct {
    bool attempted;
    bool ok;
    int http_status;
    esp_err_t err;
    int tls_err;
    uint32_t cert_flags;
    int sock_errno;
    uint32_t latency_ms;
    int last_ok_age_sec;
    char endpoint[48];
    char stage[16];
    char detail[128];
} cs_api_last_status_t;

typedef struct {
    char url[192];
    bool url_valid;
    bool is_https;
    char host[128];
    uint16_t port;

    bool wifi_connected;
    char wifi_ssid[33];
    char ip[16];
    char dns_server[16];
    int rssi;

    bool dns_ok;
    char resolved_ip[46];
    uint32_t dns_ms;
    int dns_err;

    bool apps_ok;
    int apps_status;
    esp_err_t apps_err;
    uint32_t apps_ms;
    int tls_err;
    uint32_t cert_flags;
    int sock_errno;
    char redirect_location[192];

    bool config_ok;
    int config_status;
    esp_err_t config_err;
    uint32_t config_ms;

    char stage[16];
    char hint[384];
} cs_server_diag_t;

/**
 * Snapshot the most recent server API call outcome for UI diagnostics.
 */
void api_client_get_last_status(cs_api_last_status_t *out);

/**
 * Run a step-by-step live connectivity diagnostic (URL parse, Wi-Fi/DNS,
 * /api/v1/apps, and /api/v1/device/{id}/config) without modifying NVS settings.
 * Pass override_base_url=NULL or "" to test the saved api_base_url.
 */
esp_err_t api_client_diagnose_server(const char *override_base_url, cs_server_diag_t *out);

#ifdef __cplusplus
}
#endif
