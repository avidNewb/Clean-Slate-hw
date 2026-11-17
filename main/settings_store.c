#include "settings_store.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "settings";
static const char *NVS_NS = "cs_cfg";

static SemaphoreHandle_t s_mutex = NULL;
static cs_local_settings_t s_cfg;

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
    cfg->fight_folder = 0;
#endif
#ifdef CONFIG_CS_FIGHT_SONG_TRACK
    cfg->fight_track = CONFIG_CS_FIGHT_SONG_TRACK;
#else
    cfg->fight_track = 1;
#endif
#ifdef CONFIG_CS_VICTORY_SONG_FOLDER
    cfg->victory_folder = CONFIG_CS_VICTORY_SONG_FOLDER;
#else
    cfg->victory_folder = 0;
#endif
#ifdef CONFIG_CS_VICTORY_SONG_TRACK
    cfg->victory_track = CONFIG_CS_VICTORY_SONG_TRACK;
#else
    cfg->victory_track = 2;
#endif
#ifdef CONFIG_CS_THIRD_SONG_FOLDER
    cfg->third_folder = CONFIG_CS_THIRD_SONG_FOLDER;
#else
    cfg->third_folder = 0;
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
        nvs_close(h);
    }

    trim_trailing_slash(tmp.api_base_url);
    if (!tmp.api_base_url[0]) {
        snprintf(tmp.api_base_url, sizeof(tmp.api_base_url), "%s", CONFIG_CS_API_BASE_URL);
        trim_trailing_slash(tmp.api_base_url);
    }
    normalize_teams(tmp.favorite_teams);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_cfg = tmp;
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

    nvs_handle_t h = 0;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs_open failed");

    esp_err_t err = ESP_OK;
    if (err == ESP_OK) err = nvs_set_str(h, "wifi_ssid", clean.wifi_ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "wifi_pass", clean.wifi_pass);
    if (err == ESP_OK) err = nvs_set_str(h, "api_url", clean.api_base_url);
    if (err == ESP_OK) err = nvs_set_str(h, "dev_name", clean.device_name);
    if (err == ESP_OK) err = nvs_set_str(h, "fav_teams", clean.favorite_teams);
    if (err == ESP_OK) err = nvs_set_str(h, "timezone", clean.timezone);
    if (err == ESP_OK) err = nvs_set_str(h, "ntp_srv", clean.ntp_server);
    if (err == ESP_OK) err = nvs_set_str(h, "q_start", clean.quiet_start);
    if (err == ESP_OK) err = nvs_set_str(h, "q_end", clean.quiet_end);
    if (err == ESP_OK) err = nvs_set_u8(h, "q_en", clean.quiet_enabled ? 1U : 0U);
    if (err == ESP_OK) err = nvs_set_i32(h, "bright_ov", clean.brightness_override);
    if (err == ESP_OK) err = nvs_set_u8(h, "bright_mn", clean.brightness_min);
    if (err == ESP_OK) err = nvs_set_u8(h, "bright_mx", clean.brightness_max);
    if (err == ESP_OK) err = nvs_set_u8(h, "df_vol", clean.df_volume);
    if (err == ESP_OK) err = nvs_set_u8(h, "df_eq", clean.df_eq);
    if (err == ESP_OK) err = nvs_set_u8(h, "boot_snd", clean.boot_tone ? 1U : 0U);
    if (err == ESP_OK) err = nvs_set_u8(h, "chime_snd", clean.hourly_chime ? 1U : 0U);
    if (err == ESP_OK) err = nvs_set_u16(h, "rot_idle", clean.rotary_idle_sec);
    if (err == ESP_OK) err = nvs_set_u16(h, "poll_sec", clean.poll_interval_sec);
    if (err == ESP_OK) err = nvs_set_u8(h, "ota_en", clean.ota_enabled ? 1U : 0U);
    if (err == ESP_OK) err = nvs_set_u8(h, "fg_fld", clean.fight_folder);
    if (err == ESP_OK) err = nvs_set_u16(h, "fg_trk", clean.fight_track);
    if (err == ESP_OK) err = nvs_set_u8(h, "vc_fld", clean.victory_folder);
    if (err == ESP_OK) err = nvs_set_u16(h, "vc_trk", clean.victory_track);
    if (err == ESP_OK) err = nvs_set_u8(h, "th_fld", clean.third_folder);
    if (err == ESP_OK) err = nvs_set_u16(h, "th_trk", clean.third_track);
    if (err == ESP_OK) err = nvs_set_str(h, "pin_app", clean.pinned_app);
    if (err == ESP_OK) err = nvs_set_u8(h, "sync_pend", clean.cloud_sync_pending ? 1U : 0U);
    if (err == ESP_OK) err = nvs_commit(h);

    nvs_close(h);
    ESP_RETURN_ON_ERROR(err, TAG, "nvs save failed");

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_cfg = clean;
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
    xSemaphoreGive(s_mutex);
    ESP_LOGW(TAG, "Factory reset complete (NVS cleared)");
    return err;
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
