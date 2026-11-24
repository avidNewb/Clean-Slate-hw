#include "settings_store.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "json_min.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "settings";
static const char *NVS_NS = "cs_cfg";

static SemaphoreHandle_t s_mutex = NULL;
static cs_local_settings_t s_cfg;
static cs_alarm_schedule_t s_alarms;
static bool s_nvs_has_baseline = false;

static void trim_trailing_slash(char *url)
{
    if (!url || !*url) {
        return;
    }
    char *start = url;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != url) {
        memmove(url, start, strlen(start) + 1U);
    }
    size_t n = strlen(url);
    while (n > 0 && (isspace((unsigned char)url[n - 1]) || url[n - 1] == '/')) {
        url[--n] = '\0';
    }
}

static void normalize_teams(char *teams)
{
    char clean[sizeof(s_cfg.favorite_teams)] = {0};
    size_t w = 0;
    for (size_t i = 0; teams[i] != '\0' && w + 1 < sizeof(clean); i++) {
        unsigned char c = (unsigned char)teams[i];
        if (isalnum(c)) {
            clean[w++] = (char)toupper(c);
        } else if (c == ',' && w > 0 && clean[w - 1] != ',') {
            clean[w++] = ',';
        }
    }
    if (w > 0 && clean[w - 1] == ',') {
        clean[w - 1] = '\0';
    }
    snprintf(teams, sizeof(s_cfg.favorite_teams), "%s", clean);
}

static void load_defaults(cs_local_settings_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    if (strlen(CONFIG_CS_WIFI_SSID) > 0 && strcmp(CONFIG_CS_WIFI_SSID, "MyNetwork") != 0) {
        snprintf(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), "%s", CONFIG_CS_WIFI_SSID);
        snprintf(cfg->wifi_pass, sizeof(cfg->wifi_pass), "%s", CONFIG_CS_WIFI_PASSWORD);
    }
    snprintf(cfg->api_base_url, sizeof(cfg->api_base_url), "%s", CONFIG_CS_API_BASE_URL);
    trim_trailing_slash(cfg->api_base_url);
    snprintf(cfg->device_name, sizeof(cfg->device_name), "Clean Slate");
    snprintf(cfg->favorite_teams, sizeof(cfg->favorite_teams), "ALA");
    snprintf(cfg->timezone, sizeof(cfg->timezone), "%s", CONFIG_CS_DEFAULT_TIMEZONE);
    cfg->quiet_enabled = false;
    snprintf(cfg->quiet_start, sizeof(cfg->quiet_start), "23:00");
    snprintf(cfg->quiet_end, sizeof(cfg->quiet_end), "07:00");
    cfg->brightness_override = -1;
#ifdef CONFIG_CS_HUB75_MIN_BRIGHTNESS
    cfg->brightness_min = (uint8_t)CONFIG_CS_HUB75_MIN_BRIGHTNESS;
#else
    cfg->brightness_min = 15;
#endif
#ifdef CONFIG_CS_HUB75_MAX_BRIGHTNESS
    cfg->brightness_max = (uint8_t)CONFIG_CS_HUB75_MAX_BRIGHTNESS;
#else
    cfg->brightness_max = 255;
#endif
    if (cfg->brightness_max < cfg->brightness_min) {
        cfg->brightness_max = cfg->brightness_min;
    }
#ifdef CONFIG_CS_DFPLAYER_DEFAULT_VOLUME
    cfg->df_volume = CONFIG_CS_DFPLAYER_DEFAULT_VOLUME;
#else
    cfg->df_volume = 18;
#endif
    cfg->df_eq = 0;
#ifdef CONFIG_CS_PLAY_BOOT_TONE
    cfg->boot_tone = true;
#else
    cfg->boot_tone = false;
#endif
#ifdef CONFIG_CS_PLAY_HOURLY_CHIME
    cfg->hourly_chime = true;
#else
    cfg->hourly_chime = false;
#endif
#ifdef CONFIG_CS_ROTARY_IDLE_SEC
    cfg->rotary_idle_sec = CONFIG_CS_ROTARY_IDLE_SEC;
#else
    cfg->rotary_idle_sec = 30;
#endif
#ifdef CONFIG_CS_API_POLL_INTERVAL_SEC
    cfg->poll_interval_sec = CONFIG_CS_API_POLL_INTERVAL_SEC;
#else
    cfg->poll_interval_sec = 15;
#endif
#ifdef CONFIG_CS_OTA_ENABLED
    cfg->ota_enabled = true;
#else
    cfg->ota_enabled = false;
#endif
    snprintf(cfg->ntp_server, sizeof(cfg->ntp_server), "%s", CONFIG_CS_NTP_SERVER);
#ifdef CONFIG_CS_FIGHT_SONG_FOLDER
    cfg->fight_folder = CONFIG_CS_FIGHT_SONG_FOLDER;
#else
    cfg->fight_folder = 1;
#endif
#ifdef CONFIG_CS_FIGHT_SONG_TRACK
    cfg->fight_track = CONFIG_CS_FIGHT_SONG_TRACK;
#else
    cfg->fight_track = 1;
#endif
#ifdef CONFIG_CS_VICTORY_SONG_FOLDER
    cfg->victory_folder = CONFIG_CS_VICTORY_SONG_FOLDER;
#else
    cfg->victory_folder = 1;
#endif
#ifdef CONFIG_CS_VICTORY_SONG_TRACK
    cfg->victory_track = CONFIG_CS_VICTORY_SONG_TRACK;
#else
    cfg->victory_track = 2;
#endif
#ifdef CONFIG_CS_THIRD_SONG_FOLDER
    cfg->third_folder = CONFIG_CS_THIRD_SONG_FOLDER;
#else
    cfg->third_folder = 1;
#endif
#ifdef CONFIG_CS_THIRD_SONG_TRACK
    cfg->third_track = CONFIG_CS_THIRD_SONG_TRACK;
#else
    cfg->third_track = 3;
#endif
    cfg->cloud_sync_pending = false;
}

static void nvs_read_str_opt(nvs_handle_t h, const char *key, char *dst, size_t cap)
{
    size_t len = cap;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) {
        return;
    }
    dst[cap - 1] = '\0';
}

esp_err_t settings_store_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_mutex != NULL, ESP_ERR_NO_MEM, TAG, "mutex alloc failed");
    }

    cs_local_settings_t tmp;
    load_defaults(&tmp);

    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        nvs_read_str_opt(h, "wifi_ssid", tmp.wifi_ssid, sizeof(tmp.wifi_ssid));
        nvs_read_str_opt(h, "wifi_pass", tmp.wifi_pass, sizeof(tmp.wifi_pass));
        nvs_read_str_opt(h, "api_url", tmp.api_base_url, sizeof(tmp.api_base_url));
        nvs_read_str_opt(h, "dev_name", tmp.device_name, sizeof(tmp.device_name));
        nvs_read_str_opt(h, "fav_teams", tmp.favorite_teams, sizeof(tmp.favorite_teams));
        nvs_read_str_opt(h, "timezone", tmp.timezone, sizeof(tmp.timezone));
        nvs_read_str_opt(h, "ntp_srv", tmp.ntp_server, sizeof(tmp.ntp_server));
        nvs_read_str_opt(h, "q_start", tmp.quiet_start, sizeof(tmp.quiet_start));
        nvs_read_str_opt(h, "q_end", tmp.quiet_end, sizeof(tmp.quiet_end));
        nvs_read_str_opt(h, "pin_app", tmp.pinned_app, sizeof(tmp.pinned_app));
        nvs_read_str_opt(h, "dev_tok", tmp.device_token, sizeof(tmp.device_token));

        uint8_t u8 = 0;
        uint16_t u16 = 0;
        int32_t i32 = -1;
        if (nvs_get_u8(h, "q_en", &u8) == ESP_OK) {
            tmp.quiet_enabled = (u8 != 0);
        }
        if (nvs_get_i32(h, "bright_ov", &i32) == ESP_OK) {
            tmp.brightness_override = (i32 >= 0 && i32 <= 255) ? (int)i32 : -1;
        }
        if (nvs_get_u8(h, "bright_mn", &u8) == ESP_OK && u8 >= 1U) {
            tmp.brightness_min = u8;
        }
        if (nvs_get_u8(h, "bright_mx", &u8) == ESP_OK && u8 >= 1U) {
            tmp.brightness_max = u8;
        }
        if (tmp.brightness_max < tmp.brightness_min) {
            tmp.brightness_max = tmp.brightness_min;
        }
        if (nvs_get_u8(h, "df_vol", &u8) == ESP_OK && u8 <= 30) {
            tmp.df_volume = u8;
        }
        if (nvs_get_u8(h, "df_eq", &u8) == ESP_OK && u8 <= 5) {
            tmp.df_eq = u8;
        }
        if (nvs_get_u8(h, "boot_snd", &u8) == ESP_OK) {
            tmp.boot_tone = (u8 != 0);
        }
        if (nvs_get_u8(h, "chime_snd", &u8) == ESP_OK) {
            tmp.hourly_chime = (u8 != 0);
        }
        if (nvs_get_u16(h, "rot_idle", &u16) == ESP_OK && u16 <= 600) {
            tmp.rotary_idle_sec = u16;
        }
        if (nvs_get_u16(h, "poll_sec", &u16) == ESP_OK && u16 >= 1 && u16 <= 3600) {
            tmp.poll_interval_sec = u16;
        }
        if (nvs_get_u8(h, "ota_en", &u8) == ESP_OK) {
            tmp.ota_enabled = (u8 != 0);
        }
        if (nvs_get_u8(h, "fg_fld", &u8) == ESP_OK && u8 <= 99) {
            tmp.fight_folder = u8;
        }
        if (nvs_get_u16(h, "fg_trk", &u16) == ESP_OK && u16 >= 1 && u16 <= 2999) {
            tmp.fight_track = u16;
        }
        if (nvs_get_u8(h, "vc_fld", &u8) == ESP_OK && u8 <= 99) {
            tmp.victory_folder = u8;
        }
        if (nvs_get_u16(h, "vc_trk", &u16) == ESP_OK && u16 >= 1 && u16 <= 2999) {
            tmp.victory_track = u16;
        }
        if (nvs_get_u8(h, "th_fld", &u8) == ESP_OK && u8 <= 99) {
            tmp.third_folder = u8;
        }
        if (nvs_get_u16(h, "th_trk", &u16) == ESP_OK && u16 >= 1 && u16 <= 2999) {
            tmp.third_track = u16;
        }
        if (nvs_get_u8(h, "sync_pend", &u8) == ESP_OK) {
            tmp.cloud_sync_pending = (u8 != 0);
        }
        typedef struct {
            bool enabled;
            bool repeat;
            uint8_t hour;
            uint8_t minute;
            uint8_t days_mask;
            uint8_t folder;
            uint8_t track;
            uint8_t theme_id;
            char id[25];
            char label[25];
        } cs_alarm_item_v2_t;
        typedef struct {
            bool app_enabled;
            bool allow_during_quiet_hours;
            bool use_24h;
            uint8_t duration_minutes;
            uint8_t volume;
            uint8_t count;
            cs_alarm_item_v2_t items[CS_MAX_LOCAL_ALARMS];
            uint8_t pending_delete_count;
            char pending_delete_ids[CS_MAX_LOCAL_ALARMS][25];
        } cs_alarm_schedule_v2_t;

        cs_alarm_schedule_t loaded_alarms;
        memset(&loaded_alarms, 0, sizeof(loaded_alarms));
        loaded_alarms.allow_during_quiet_hours = true;
        loaded_alarms.duration_minutes = 2;
        loaded_alarms.volume = 26;
        size_t blob_len = sizeof(loaded_alarms);
        if (nvs_get_blob(h, "alarms_v3", &loaded_alarms, &blob_len) == ESP_OK &&
            blob_len == sizeof(loaded_alarms) &&
            loaded_alarms.count <= CS_MAX_LOCAL_ALARMS &&
            loaded_alarms.pending_delete_count <= CS_MAX_LOCAL_ALARMS) {
            s_alarms = loaded_alarms;
        } else {
            cs_alarm_schedule_v2_t v2;
            size_t v2_len = sizeof(v2);
            memset(&s_alarms, 0, sizeof(s_alarms));
            s_alarms.allow_during_quiet_hours = true;
            s_alarms.duration_minutes = 2;
            s_alarms.volume = 26;
            if (nvs_get_blob(h, "alarms_v2", &v2, &v2_len) == ESP_OK &&
                v2_len == sizeof(v2) &&
                v2.count <= CS_MAX_LOCAL_ALARMS &&
                v2.pending_delete_count <= CS_MAX_LOCAL_ALARMS) {
                s_alarms.app_enabled = v2.app_enabled;
                s_alarms.allow_during_quiet_hours = v2.allow_during_quiet_hours;
                s_alarms.use_24h = v2.use_24h;
                s_alarms.duration_minutes = v2.duration_minutes;
                s_alarms.volume = v2.volume;
                s_alarms.count = v2.count;
                for (uint8_t i = 0; i < v2.count; i++) {
                    s_alarms.items[i].enabled = v2.items[i].enabled;
                    s_alarms.items[i].repeat = v2.items[i].repeat;
                    s_alarms.items[i].hour = v2.items[i].hour;
                    s_alarms.items[i].minute = v2.items[i].minute;
                    s_alarms.items[i].days_mask = v2.items[i].days_mask;
                    s_alarms.items[i].folder = v2.items[i].folder;
                    s_alarms.items[i].track = v2.items[i].track;
                    s_alarms.items[i].theme_id = v2.items[i].theme_id;
                    memcpy(s_alarms.items[i].id, v2.items[i].id, sizeof(s_alarms.items[i].id));
                    memcpy(s_alarms.items[i].label, v2.items[i].label, sizeof(s_alarms.items[i].label));
                    s_alarms.items[i].post_action = CS_ALARM_POST_NONE;
                    s_alarms.items[i].post_minutes = 5;
                }
                s_alarms.pending_delete_count = v2.pending_delete_count;
                memcpy(s_alarms.pending_delete_ids, v2.pending_delete_ids,
                       sizeof(s_alarms.pending_delete_ids));
                ESP_LOGI(TAG, "Migrated %u local alarm(s) from alarms_v2 to alarms_v3",
                         (unsigned)s_alarms.count);
            }
        }
        nvs_close(h);
    } else {
        memset(&s_alarms, 0, sizeof(s_alarms));
        s_alarms.allow_during_quiet_hours = true;
        s_alarms.duration_minutes = 2;
        s_alarms.volume = 26;
    }

    trim_trailing_slash(tmp.api_base_url);
    if (!tmp.api_base_url[0]) {
        snprintf(tmp.api_base_url, sizeof(tmp.api_base_url), "%s", CONFIG_CS_API_BASE_URL);
        trim_trailing_slash(tmp.api_base_url);
    }
    normalize_teams(tmp.favorite_teams);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_cfg = tmp;
    s_nvs_has_baseline = (err == ESP_OK);
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Loaded settings (wifi_configured=%s, teams=%s, api=%s)",
             tmp.wifi_ssid[0] ? "yes" : "no",
             tmp.favorite_teams[0] ? tmp.favorite_teams : "(none)",
             tmp.api_base_url);
    return ESP_OK;
}

void settings_store_get(cs_local_settings_t *out)
{
    if (!out) {
        return;
    }
    if (!s_mutex) {
        load_defaults(out);
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_mutex);
}

esp_err_t settings_store_save(const cs_local_settings_t *in)
{
    ESP_RETURN_ON_FALSE(in != NULL, ESP_ERR_INVALID_ARG, TAG, "null settings");
    ESP_RETURN_ON_FALSE(s_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    cs_local_settings_t clean = *in;
    trim_trailing_slash(clean.api_base_url);
    if (!clean.api_base_url[0]) {
        snprintf(clean.api_base_url, sizeof(clean.api_base_url), "%s", CONFIG_CS_API_BASE_URL);
        trim_trailing_slash(clean.api_base_url);
    }
    if (!clean.device_name[0]) {
        snprintf(clean.device_name, sizeof(clean.device_name), "Clean Slate");
    }
    if (!clean.timezone[0]) {
        snprintf(clean.timezone, sizeof(clean.timezone), "%s", CONFIG_CS_DEFAULT_TIMEZONE);
    }
    if (!clean.ntp_server[0]) {
        snprintf(clean.ntp_server, sizeof(clean.ntp_server), "%s", CONFIG_CS_NTP_SERVER);
    }
    if (!clean.quiet_start[0]) {
        snprintf(clean.quiet_start, sizeof(clean.quiet_start), "23:00");
    }
    if (!clean.quiet_end[0]) {
        snprintf(clean.quiet_end, sizeof(clean.quiet_end), "07:00");
    }
    normalize_teams(clean.favorite_teams);
    if (clean.brightness_override > 255) {
        clean.brightness_override = 255;
    } else if (clean.brightness_override < 0) {
        clean.brightness_override = -1;
    }
    if (clean.brightness_min < 1U) {
        clean.brightness_min = 1U;
    }
    if (clean.brightness_max < clean.brightness_min) {
        clean.brightness_max = clean.brightness_min;
    }
    if (clean.df_volume > 30) {
        clean.df_volume = 30;
    }
    if (clean.df_eq > 5) {
        clean.df_eq = 0;
    }
    if (clean.rotary_idle_sec > 600) {
        clean.rotary_idle_sec = 600;
    }
    if (clean.poll_interval_sec < 1U) {
        clean.poll_interval_sec = 1U;
    } else if (clean.poll_interval_sec > 3600U) {
        clean.poll_interval_sec = 3600U;
    }
    if (clean.fight_folder > 99) {
        clean.fight_folder = 99;
    }
    if (clean.fight_track == 0) {
        clean.fight_track = 1;
    } else if (clean.fight_track > 2999) {
        clean.fight_track = 2999;
    }
    if (clean.victory_folder > 99) {
        clean.victory_folder = 99;
    }
    if (clean.victory_track == 0) {
        clean.victory_track = 2;
    } else if (clean.victory_track > 2999) {
        clean.victory_track = 2999;
    }
    if (clean.third_folder > 99) {
        clean.third_folder = 99;
    }
    if (clean.third_track == 0) {
        clean.third_track = 3;
    } else if (clean.third_track > 2999) {
        clean.third_track = 2999;
    }

    bool dirty_wifi_ssid, dirty_wifi_pass, dirty_api_url, dirty_dev_name;
    bool dirty_fav_teams, dirty_timezone, dirty_ntp_srv, dirty_q_start;
    bool dirty_q_end, dirty_q_en, dirty_bright_ov, dirty_bright_mn;
    bool dirty_bright_mx, dirty_df_vol, dirty_df_eq, dirty_boot_snd;
    bool dirty_chime_snd, dirty_rot_idle, dirty_poll_sec, dirty_ota_en;
    bool dirty_fg_fld, dirty_fg_trk, dirty_vc_fld, dirty_vc_trk;
    bool dirty_th_fld, dirty_th_trk, dirty_pin_app, dirty_dev_tok, dirty_sync_pend;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool diff_mode = s_nvs_has_baseline;
    dirty_wifi_ssid = (!diff_mode || strcmp(clean.wifi_ssid, s_cfg.wifi_ssid) != 0);
    dirty_wifi_pass = (!diff_mode || strcmp(clean.wifi_pass, s_cfg.wifi_pass) != 0);
    dirty_api_url   = (!diff_mode || strcmp(clean.api_base_url, s_cfg.api_base_url) != 0);
    dirty_dev_name  = (!diff_mode || strcmp(clean.device_name, s_cfg.device_name) != 0);
    dirty_fav_teams = (!diff_mode || strcmp(clean.favorite_teams, s_cfg.favorite_teams) != 0);
    dirty_timezone  = (!diff_mode || strcmp(clean.timezone, s_cfg.timezone) != 0);
    dirty_ntp_srv   = (!diff_mode || strcmp(clean.ntp_server, s_cfg.ntp_server) != 0);
    dirty_q_start   = (!diff_mode || strcmp(clean.quiet_start, s_cfg.quiet_start) != 0);
    dirty_q_end     = (!diff_mode || strcmp(clean.quiet_end, s_cfg.quiet_end) != 0);
    dirty_q_en      = (!diff_mode || clean.quiet_enabled != s_cfg.quiet_enabled);
    dirty_bright_ov = (!diff_mode || clean.brightness_override != s_cfg.brightness_override);
    dirty_bright_mn = (!diff_mode || clean.brightness_min != s_cfg.brightness_min);
    dirty_bright_mx = (!diff_mode || clean.brightness_max != s_cfg.brightness_max);
    dirty_df_vol    = (!diff_mode || clean.df_volume != s_cfg.df_volume);
    dirty_df_eq     = (!diff_mode || clean.df_eq != s_cfg.df_eq);
    dirty_boot_snd  = (!diff_mode || clean.boot_tone != s_cfg.boot_tone);
    dirty_chime_snd = (!diff_mode || clean.hourly_chime != s_cfg.hourly_chime);
    dirty_rot_idle  = (!diff_mode || clean.rotary_idle_sec != s_cfg.rotary_idle_sec);
    dirty_poll_sec  = (!diff_mode || clean.poll_interval_sec != s_cfg.poll_interval_sec);
    dirty_ota_en    = (!diff_mode || clean.ota_enabled != s_cfg.ota_enabled);
    dirty_fg_fld    = (!diff_mode || clean.fight_folder != s_cfg.fight_folder);
    dirty_fg_trk    = (!diff_mode || clean.fight_track != s_cfg.fight_track);
    dirty_vc_fld    = (!diff_mode || clean.victory_folder != s_cfg.victory_folder);
    dirty_vc_trk    = (!diff_mode || clean.victory_track != s_cfg.victory_track);
    dirty_th_fld    = (!diff_mode || clean.third_folder != s_cfg.third_folder);
    dirty_th_trk    = (!diff_mode || clean.third_track != s_cfg.third_track);
    dirty_pin_app   = (!diff_mode || strcmp(clean.pinned_app, s_cfg.pinned_app) != 0);
    dirty_dev_tok   = (!diff_mode || strcmp(clean.device_token, s_cfg.device_token) != 0);
    dirty_sync_pend = (!diff_mode || clean.cloud_sync_pending != s_cfg.cloud_sync_pending);
    xSemaphoreGive(s_mutex);

    bool any_dirty =
        dirty_wifi_ssid || dirty_wifi_pass || dirty_api_url || dirty_dev_name ||
        dirty_fav_teams || dirty_timezone || dirty_ntp_srv || dirty_q_start ||
        dirty_q_end || dirty_q_en || dirty_bright_ov || dirty_bright_mn ||
        dirty_bright_mx || dirty_df_vol || dirty_df_eq || dirty_boot_snd ||
        dirty_chime_snd || dirty_rot_idle || dirty_poll_sec || dirty_ota_en ||
        dirty_fg_fld || dirty_fg_trk || dirty_vc_fld || dirty_vc_trk ||
        dirty_th_fld || dirty_th_trk || dirty_pin_app || dirty_dev_tok || dirty_sync_pend;

    if (!any_dirty) {
        return ESP_OK;
    }

    nvs_handle_t h = 0;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs_open failed");

    esp_err_t err = ESP_OK;
    if (err == ESP_OK && dirty_wifi_ssid) err = nvs_set_str(h, "wifi_ssid", clean.wifi_ssid);
    if (err == ESP_OK && dirty_wifi_pass) err = nvs_set_str(h, "wifi_pass", clean.wifi_pass);
    if (err == ESP_OK && dirty_api_url)   err = nvs_set_str(h, "api_url", clean.api_base_url);
    if (err == ESP_OK && dirty_dev_name)  err = nvs_set_str(h, "dev_name", clean.device_name);
    if (err == ESP_OK && dirty_fav_teams) err = nvs_set_str(h, "fav_teams", clean.favorite_teams);
    if (err == ESP_OK && dirty_timezone)  err = nvs_set_str(h, "timezone", clean.timezone);
    if (err == ESP_OK && dirty_ntp_srv)   err = nvs_set_str(h, "ntp_srv", clean.ntp_server);
    if (err == ESP_OK && dirty_q_start)   err = nvs_set_str(h, "q_start", clean.quiet_start);
    if (err == ESP_OK && dirty_q_end)     err = nvs_set_str(h, "q_end", clean.quiet_end);
    if (err == ESP_OK && dirty_q_en)      err = nvs_set_u8(h, "q_en", clean.quiet_enabled ? 1U : 0U);
    if (err == ESP_OK && dirty_bright_ov) err = nvs_set_i32(h, "bright_ov", clean.brightness_override);
    if (err == ESP_OK && dirty_bright_mn) err = nvs_set_u8(h, "bright_mn", clean.brightness_min);
    if (err == ESP_OK && dirty_bright_mx) err = nvs_set_u8(h, "bright_mx", clean.brightness_max);
    if (err == ESP_OK && dirty_df_vol)    err = nvs_set_u8(h, "df_vol", clean.df_volume);
    if (err == ESP_OK && dirty_df_eq)     err = nvs_set_u8(h, "df_eq", clean.df_eq);
    if (err == ESP_OK && dirty_boot_snd)  err = nvs_set_u8(h, "boot_snd", clean.boot_tone ? 1U : 0U);
    if (err == ESP_OK && dirty_chime_snd) err = nvs_set_u8(h, "chime_snd", clean.hourly_chime ? 1U : 0U);
    if (err == ESP_OK && dirty_rot_idle)  err = nvs_set_u16(h, "rot_idle", clean.rotary_idle_sec);
    if (err == ESP_OK && dirty_poll_sec)  err = nvs_set_u16(h, "poll_sec", clean.poll_interval_sec);
    if (err == ESP_OK && dirty_ota_en)    err = nvs_set_u8(h, "ota_en", clean.ota_enabled ? 1U : 0U);
    if (err == ESP_OK && dirty_fg_fld)    err = nvs_set_u8(h, "fg_fld", clean.fight_folder);
    if (err == ESP_OK && dirty_fg_trk)    err = nvs_set_u16(h, "fg_trk", clean.fight_track);
    if (err == ESP_OK && dirty_vc_fld)    err = nvs_set_u8(h, "vc_fld", clean.victory_folder);
    if (err == ESP_OK && dirty_vc_trk)    err = nvs_set_u16(h, "vc_trk", clean.victory_track);
    if (err == ESP_OK && dirty_th_fld)    err = nvs_set_u8(h, "th_fld", clean.third_folder);
    if (err == ESP_OK && dirty_th_trk)    err = nvs_set_u16(h, "th_trk", clean.third_track);
    if (err == ESP_OK && dirty_pin_app)   err = nvs_set_str(h, "pin_app", clean.pinned_app);
    if (err == ESP_OK && dirty_dev_tok)   err = nvs_set_str(h, "dev_tok", clean.device_token);
    if (err == ESP_OK && dirty_sync_pend) err = nvs_set_u8(h, "sync_pend", clean.cloud_sync_pending ? 1U : 0U);
    if (err == ESP_OK) err = nvs_commit(h);

    nvs_close(h);
    ESP_RETURN_ON_ERROR(err, TAG, "nvs save failed");

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_cfg = clean;
    s_nvs_has_baseline = true;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t settings_store_factory_reset(void)
{
    ESP_RETURN_ON_FALSE(s_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_all(h);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }

    cs_local_settings_t def;
    load_defaults(&def);
    def.wifi_ssid[0] = '\0';
    def.wifi_pass[0] = '\0';

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_cfg = def;
    s_nvs_has_baseline = false;
    memset(&s_alarms, 0, sizeof(s_alarms));
    s_alarms.allow_during_quiet_hours = true;
    s_alarms.duration_minutes = 2;
    s_alarms.volume = 26;
    xSemaphoreGive(s_mutex);
    ESP_LOGW(TAG, "Factory reset complete (NVS cleared)");
    return err;
}

static void parse_hhmm(const char *raw, uint8_t *out_h, uint8_t *out_m)
{
    int h = 7;
    int m = 0;
    if (raw && sscanf(raw, "%d:%d", &h, &m) == 2 && h >= 0 && h <= 23 && m >= 0 && m <= 59) {
        *out_h = (uint8_t)h;
        *out_m = (uint8_t)m;
    } else {
        *out_h = 7;
        *out_m = 0;
    }
}

static void parse_sound_str(const char *raw, uint8_t def_folder, uint8_t def_track,
                            uint8_t *out_folder, uint8_t *out_track)
{
    int f = def_folder;
    int t = def_track;
    if (raw && raw[0]) {
        int pf = 0;
        int pt = 0;
        if (sscanf(raw, "%d:%d", &pf, &pt) == 2 && pf >= 1 && pf <= 99 && pt >= 1 && pt <= 255) {
            f = pf;
            t = pt;
        }
    }
    *out_folder = (uint8_t)f;
    *out_track = (uint8_t)t;
}

static uint8_t parse_theme_id(const char *raw, uint8_t def_theme)
{
    if (!raw || !raw[0]) {
        return def_theme;
    }
    if (strcmp(raw, "sunrise") == 0) return 0;
    if (strcmp(raw, "digital_pulse") == 0) return 1;
    if (strcmp(raw, "siren_strobe") == 0) return 2;
    if (strcmp(raw, "gentle_chime") == 0) return 3;
    return def_theme;
}

static uint8_t parse_days_mask(json_span_t arr)
{
    if (!arr.ptr || arr.len < 2 || arr.ptr[0] != '[') {
        return 0;
    }
    uint8_t mask = 0;
    size_t cur = 0;
    json_span_t elem;
    while (json_arr_iter(arr, &cur, &elem)) {
        char d[12] = {0};
        if (!json_as_str(elem, d, sizeof(d))) {
            continue;
        }
        for (size_t i = 0; d[i]; i++) {
            d[i] = (char)tolower((unsigned char)d[i]);
        }
        if (strcmp(d, "sun") == 0) mask |= (1U << 0);
        else if (strcmp(d, "mon") == 0) mask |= (1U << 1);
        else if (strcmp(d, "tue") == 0) mask |= (1U << 2);
        else if (strcmp(d, "wed") == 0) mask |= (1U << 3);
        else if (strcmp(d, "thu") == 0) mask |= (1U << 4);
        else if (strcmp(d, "fri") == 0) mask |= (1U << 5);
        else if (strcmp(d, "sat") == 0) mask |= (1U << 6);
    }
    return mask;
}

static void uppercase_label(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) {
        return;
    }
    if (!src || !src[0]) {
        snprintf(dst, cap, "WAKE UP!");
        return;
    }
    size_t w = 0;
    for (size_t i = 0; src[i] && w + 1 < cap; i++) {
        dst[w++] = (char)toupper((unsigned char)src[i]);
    }
    while (w > 0 && isspace((unsigned char)dst[w - 1])) {
        w--;
    }
    dst[w] = '\0';
    if (w == 0) {
        snprintf(dst, cap, "WAKE UP!");
    }
}

void settings_store_parse_alarms_json(bool app_enabled,
                                      const char *alarm_obj_json,
                                      size_t alarm_obj_len,
                                      cs_alarm_schedule_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->app_enabled = app_enabled;
    out->allow_during_quiet_hours = true;
    out->use_24h = false;
    out->duration_minutes = 2;
    out->volume = 26;

    if (!alarm_obj_json || alarm_obj_len < 2) {
        return;
    }

    json_span_t root = json_root(alarm_obj_json, alarm_obj_len);
    json_span_t fv;
    int64_t iv = 0;
    bool bv = false;

    if (json_obj_get(root, "allow_during_quiet_hours", &fv) && json_as_bool(fv, &bv)) {
        out->allow_during_quiet_hours = bv;
    }
    if (json_obj_get(root, "duration_minutes", &fv) && json_as_int(fv, &iv) && iv >= 1 && iv <= 15) {
        out->duration_minutes = (uint8_t)iv;
    }
    if (json_obj_get(root, "alarm_volume", &fv) && json_as_int(fv, &iv) && iv >= 0 && iv <= 30) {
        out->volume = (uint8_t)iv;
    }
    char tf[12] = {0};
    if (json_obj_get(root, "time_format", &fv) && json_as_str(fv, tf, sizeof(tf))) {
        out->use_24h = (strcmp(tf, "24h") == 0);
    }

    uint8_t def_folder = 4, def_track = 1;
    char snd_buf[16] = {0};
    if (json_obj_get(root, "alarm_sound", &fv) && json_as_str(fv, snd_buf, sizeof(snd_buf))) {
        parse_sound_str(snd_buf, 4, 1, &def_folder, &def_track);
    }

    uint8_t def_theme = 0;
    char thm_buf[24] = {0};
    if (json_obj_get(root, "alarm_theme", &fv) && json_as_str(fv, thm_buf, sizeof(thm_buf))) {
        def_theme = parse_theme_id(thm_buf, 0);
    }

    json_span_t alarms_arr;
    bool has_alarms_key = json_obj_get(root, "alarms", &alarms_arr) && !json_is_null(alarms_arr) &&
                          alarms_arr.ptr && alarms_arr.len >= 2 && alarms_arr.ptr[0] == '[';

    if (has_alarms_key) {
        size_t cur = 0;
        json_span_t item_obj;
        while (out->count < CS_MAX_LOCAL_ALARMS && json_arr_iter(alarms_arr, &cur, &item_obj)) {
            if (!item_obj.ptr || item_obj.len < 2 || item_obj.ptr[0] != '{') {
                continue;
            }
            cs_alarm_item_t *it = &out->items[out->count];
            memset(it, 0, sizeof(*it));
            it->enabled = true;
            if (json_obj_get(item_obj, "enabled", &fv) && json_as_bool(fv, &bv)) {
                it->enabled = bv;
            }
            char idbuf[25] = {0};
            if (json_obj_get(item_obj, "id", &fv) && json_as_str(fv, idbuf, sizeof(idbuf)) && idbuf[0]) {
                snprintf(it->id, sizeof(it->id), "%s", idbuf);
            } else {
                snprintf(it->id, sizeof(it->id), "alarm_%u", (unsigned)(out->count + 1U));
            }
            char tbuf[16] = {0};
            if (json_obj_get(item_obj, "time", &fv)) {
                json_as_str(fv, tbuf, sizeof(tbuf));
            }
            parse_hhmm(tbuf, &it->hour, &it->minute);

            json_span_t days_span;
            if (json_obj_get(item_obj, "days", &days_span)) {
                it->days_mask = parse_days_mask(days_span);
            }
            bool has_repeat_field = json_obj_get(item_obj, "repeat", &fv) && json_as_bool(fv, &bv);
            if ((has_repeat_field && !bv) || it->days_mask == 0U) {
                it->repeat = false;
                it->days_mask = 0U;
            } else {
                it->repeat = true;
            }

            char lbuf[32] = {0};
            if (json_obj_get(item_obj, "label", &fv)) {
                json_as_str(fv, lbuf, sizeof(lbuf));
            }
            uppercase_label(it->label, sizeof(it->label), lbuf);

            char isnd[16] = {0};
            if (json_obj_get(item_obj, "sound", &fv)) {
                json_as_str(fv, isnd, sizeof(isnd));
            }
            parse_sound_str(isnd, def_folder, def_track, &it->folder, &it->track);

            char ithm[24] = {0};
            if (json_obj_get(item_obj, "theme", &fv)) {
                json_as_str(fv, ithm, sizeof(ithm));
            }
            it->theme_id = parse_theme_id(ithm, def_theme);

            char iafter[24] = {0};
            it->post_action = CS_ALARM_POST_NONE;
            if (json_obj_get(item_obj, "after_action", &fv) && json_as_str(fv, iafter, sizeof(iafter))) {
                if (strcasecmp(iafter, "bible_votd_gentle") == 0) {
                    it->post_action = CS_ALARM_POST_BIBLE_VOTD_GENTLE;
                } else if (strcasecmp(iafter, "bible_votd") == 0) {
                    it->post_action = CS_ALARM_POST_BIBLE_VOTD;
                }
            }
            it->post_minutes = 5;
            if (json_obj_get(item_obj, "after_minutes", &fv) && json_as_int(fv, &iv)) {
                if (iv < 1) {
                    iv = 1;
                } else if (iv > 60) {
                    iv = 60;
                }
                it->post_minutes = (uint8_t)iv;
            }
            out->count++;
        }
        return;
    }

    /* Legacy single-alarm fallback */
    char legacy_time[16] = {0};
    if (json_obj_get(root, "alarm_time", &fv) && json_as_str(fv, legacy_time, sizeof(legacy_time)) &&
        legacy_time[0]) {
        cs_alarm_item_t *it = &out->items[0];
        memset(it, 0, sizeof(*it));
        it->enabled = true;
        snprintf(it->id, sizeof(it->id), "default");
        parse_hhmm(legacy_time, &it->hour, &it->minute);
        json_span_t days_span;
        if (json_obj_get(root, "alarm_days", &days_span)) {
            it->days_mask = parse_days_mask(days_span);
        }
        bool has_rep = json_obj_get(root, "alarm_repeat", &fv) && json_as_bool(fv, &bv);
        if ((has_rep && !bv) || it->days_mask == 0U) {
            it->repeat = false;
            it->days_mask = 0U;
        } else {
            it->repeat = true;
        }
        char lbuf[32] = {0};
        if (json_obj_get(root, "alarm_label", &fv)) {
            json_as_str(fv, lbuf, sizeof(lbuf));
        }
        uppercase_label(it->label, sizeof(it->label), lbuf);
        it->folder = def_folder;
        it->track = def_track;
        it->theme_id = def_theme;
        out->count = 1;
    }
}

static bool is_pending_delete(const cs_alarm_schedule_t *sched, const char *alarm_id)
{
    if (!sched || !alarm_id || !alarm_id[0]) {
        return false;
    }
    for (uint8_t i = 0; i < sched->pending_delete_count && i < CS_MAX_LOCAL_ALARMS; i++) {
        if (strcmp(sched->pending_delete_ids[i], alarm_id) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t persist_alarms_blob(const cs_alarm_schedule_t *clean)
{
    nvs_handle_t h = 0;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs_open alarms failed");
    esp_err_t err = nvs_set_blob(h, "alarms_v3", clean, sizeof(*clean));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

void settings_store_get_alarms(cs_alarm_schedule_t *out)
{
    if (!out) {
        return;
    }
    if (!s_mutex) {
        memset(out, 0, sizeof(*out));
        out->allow_during_quiet_hours = true;
        out->duration_minutes = 2;
        out->volume = 26;
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_alarms;
    xSemaphoreGive(s_mutex);
}

esp_err_t settings_store_save_alarms(const cs_alarm_schedule_t *in)
{
    ESP_RETURN_ON_FALSE(in != NULL, ESP_ERR_INVALID_ARG, TAG, "null alarms");
    ESP_RETURN_ON_FALSE(s_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    cs_alarm_schedule_t clean = *in;
    if (clean.count > CS_MAX_LOCAL_ALARMS) {
        clean.count = CS_MAX_LOCAL_ALARMS;
    }
    if (clean.duration_minutes < 1 || clean.duration_minutes > 15) {
        clean.duration_minutes = 2;
    }
    if (clean.volume > 30) {
        clean.volume = 30;
    }

    bool changed = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    /* Preserve pending cloud one-time alarm disables so concurrent GET /config never re-enables them */
    clean.pending_delete_count = s_alarms.pending_delete_count;
    memcpy(clean.pending_delete_ids, s_alarms.pending_delete_ids, sizeof(clean.pending_delete_ids));
    if (clean.pending_delete_count > 0U) {
        for (uint8_t i = 0; i < clean.count; i++) {
            if (!clean.items[i].repeat && is_pending_delete(&clean, clean.items[i].id)) {
                clean.items[i].enabled = false;
            }
        }
    }

    if (memcmp(&s_alarms, &clean, sizeof(clean)) != 0) {
        s_alarms = clean;
        changed = true;
    }
    xSemaphoreGive(s_mutex);

    if (!changed) {
        return ESP_OK;
    }

    esp_err_t err = persist_alarms_blob(&clean);
    ESP_LOGI(TAG, "Synced local alarms to NVS (app_enabled=%s, count=%u, pending_del=%u, dur=%um, vol=%u)",
             clean.app_enabled ? "yes" : "no",
             (unsigned)clean.count,
             (unsigned)clean.pending_delete_count,
             (unsigned)clean.duration_minutes,
             (unsigned)clean.volume);
    return err;
}

esp_err_t settings_store_consume_onetime_alarm(const char *alarm_id)
{
    ESP_RETURN_ON_FALSE(alarm_id && alarm_id[0], ESP_ERR_INVALID_ARG, TAG, "empty alarm_id");
    ESP_RETURN_ON_FALSE(s_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    cs_alarm_schedule_t updated;
    bool changed = false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    updated = s_alarms;
    for (uint8_t i = 0; i < updated.count && i < CS_MAX_LOCAL_ALARMS; i++) {
        if (strcmp(updated.items[i].id, alarm_id) == 0 && !updated.items[i].repeat) {
            if (updated.items[i].enabled) {
                updated.items[i].enabled = false;
                changed = true;
            }
        }
    }

    if (!is_pending_delete(&updated, alarm_id) && updated.pending_delete_count < CS_MAX_LOCAL_ALARMS) {
        snprintf(updated.pending_delete_ids[updated.pending_delete_count],
                 sizeof(updated.pending_delete_ids[0]), "%s", alarm_id);
        updated.pending_delete_count++;
        changed = true;
    }
    if (changed) {
        s_alarms = updated;
    }
    xSemaphoreGive(s_mutex);

    if (!changed) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Auto-disabled one-time alarm \"%s\" locally (queued cloud disable)", alarm_id);
    return persist_alarms_blob(&updated);
}

esp_err_t settings_store_clear_all_pending_alarm_deletes(void)
{
    ESP_RETURN_ON_FALSE(s_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    cs_alarm_schedule_t updated;
    bool changed = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    updated = s_alarms;
    if (updated.pending_delete_count > 0U) {
        updated.pending_delete_count = 0;
        memset(updated.pending_delete_ids, 0, sizeof(updated.pending_delete_ids));
        s_alarms = updated;
        changed = true;
    }
    xSemaphoreGive(s_mutex);
    return changed ? persist_alarms_blob(&updated) : ESP_OK;
}

esp_err_t settings_store_clear_pending_alarm_delete(const char *alarm_id)
{
    ESP_RETURN_ON_FALSE(alarm_id && alarm_id[0], ESP_ERR_INVALID_ARG, TAG, "empty alarm_id");
    ESP_RETURN_ON_FALSE(s_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    cs_alarm_schedule_t updated;
    bool changed = false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    updated = s_alarms;
    uint8_t w = 0;
    for (uint8_t i = 0; i < updated.pending_delete_count && i < CS_MAX_LOCAL_ALARMS; i++) {
        if (strcmp(updated.pending_delete_ids[i], alarm_id) == 0) {
            changed = true;
            continue;
        }
        if (w != i) {
            memcpy(updated.pending_delete_ids[w], updated.pending_delete_ids[i],
                   sizeof(updated.pending_delete_ids[0]));
        }
        w++;
    }
    for (uint8_t i = w; i < CS_MAX_LOCAL_ALARMS; i++) {
        updated.pending_delete_ids[i][0] = '\0';
    }
    updated.pending_delete_count = w;
    if (changed) {
        s_alarms = updated;
    }
    xSemaphoreGive(s_mutex);

    return changed ? persist_alarms_blob(&updated) : ESP_OK;
}

esp_err_t settings_store_clear_wifi(void)
{
    cs_local_settings_t cur;
    settings_store_get(&cur);
    cur.wifi_ssid[0] = '\0';
    cur.wifi_pass[0] = '\0';
    return settings_store_save(&cur);
}

bool settings_store_has_wifi(void)
{
    bool has = false;
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        has = (s_cfg.wifi_ssid[0] != '\0');
        xSemaphoreGive(s_mutex);
    }
    return has;
}

uint32_t settings_store_alarm_repeat_interval_ms(uint8_t folder, uint8_t track)
{
    switch (folder) {
    case 4:
        switch (track) {
        case 1: return 10100U; /* /04/001.mp3 Morning Clock Alarm (~9.72s) */
        case 2: return 2000U;  /* /04/002.mp3 Game Wave Alarm (~1.67s) */
        case 3: return 8400U;  /* /04/003.mp3 Digital Clock Buzzer (~8.07s) */
        case 4: return 25800U; /* /04/004.mp3 Retro Emergency Alarm (~25.50s) */
        case 5: return 4500U;  /* /04/005.mp3 Morning Rooster (~4.18s) */
        case 6: return 18400U; /* /04/006.mp3 Facility Alarm (~18.05s) */
        case 7: return 5400U;  /* /04/007.mp3 Classic Alarm (~5.09s) */
        default: return 10000U;
        }
    case 3:
        switch (track) {
        case 1: return 2300U;  /* /03/001.mp3 Classic Chime (~1.93s) */
        case 2: return 1600U;  /* /03/002.mp3 Boot Chime (~1.25s) */
        default: return 2300U;
        }
    case 2:
        switch (track) {
        case 1: return 2300U;  /* /02/001.mp3 Gameday Horn (~1.93s) */
        case 2: return 2400U;  /* /02/002.mp3 Cannon Blast (~1.99s) */
        case 3: return 1900U;  /* /02/003.mp3 Success Chime (~1.54s) */
        case 4: return 2000U;  /* /02/004.mp3 Urgent Siren (~1.59s) */
        default: return 2400U;
        }
    case 1:
        return 60000U; /* User team fight songs (UART 0x3D repeats right at song end) */
    default:
        return 3500U;
    }
}

void settings_store_get_api_base_url(char *out, size_t out_cap)
{
    if (!out || out_cap == 0) {
        return;
    }
    if (!s_mutex) {
        snprintf(out, out_cap, "%s", CONFIG_CS_API_BASE_URL);
        trim_trailing_slash(out);
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snprintf(out, out_cap, "%s", s_cfg.api_base_url);
    xSemaphoreGive(s_mutex);
}

void settings_store_get_device_token(char *out, size_t out_cap)
{
    if (!out || out_cap == 0) {
        return;
    }
    out[0] = '\0';
    if (!s_mutex) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snprintf(out, out_cap, "%s", s_cfg.device_token);
    xSemaphoreGive(s_mutex);
}

esp_err_t settings_store_set_device_token(const char *token)
{
    cs_local_settings_t cur;
    settings_store_get(&cur);
    snprintf(cur.device_token, sizeof(cur.device_token), "%s", token ? token : "");
    return settings_store_save(&cur);
}
