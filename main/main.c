#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "api_client.h"
#include "dfplayer.h"
#include "gfx_font.h"
#include "hub75_driver.h"
#include "peripherals.h"
#include "settings_store.h"
#include "sntp_time.h"
#include "web_portal.h"
#include "wifi_manager.h"

static const char *TAG = "main";

static SemaphoreHandle_t s_state_mutex;
static TaskHandle_t s_api_task;
static TaskHandle_t s_render_task;

typedef enum {
    MENU_NONE = 0,
    MENU_VOLUME,
    MENU_BRIGHTNESS,
    MENU_WIFI,
    MENU_TIMEZONE,
    MENU_FACTORY_RESET,
    MENU_COUNT
} local_menu_page_t;

typedef struct {
    const char *label;
    const char *iana;
} menu_tz_option_t;

static const menu_tz_option_t k_menu_tz[] = {
    { "EST",  "America/New_York" },
    { "CST",  "America/Chicago" },
    { "MST",  "America/Denver" },
    { "PST",  "America/Los_Angeles" },
    { "AKST", "America/Anchorage" },
    { "HST",  "Pacific/Honolulu" },
};

typedef struct {
    cs_device_config_t cfg;
    cs_rotary_slot_t rotary_slots[CS_MAX_ROTARY_SLOTS];
    size_t rotary_slot_count;
    int selected_app_idx; /* -1 = Auto schedule, >= 0 = rotary_slots[idx] */
    char manual_pinned_slot[64];
    bool rotary_override_active;
    int64_t last_rotary_us;
    local_menu_page_t menu_page;
    bool menu_edit;
    uint8_t menu_vol;
    int menu_bright;
    int menu_tz_idx;
    int64_t last_menu_us;
    char toast_text[24];
    int64_t toast_until_us;
    bool auto_brightness;
    bool has_frame;
    bool last_quiet_mode;
    bool switching_pending;
    char switching_label[24];
    uint16_t rotary_idle_sec;
    char active_app_id[64];
    char auto_app_id[32];
    char ota_status[24];
    char ota_detail[64];
    uint32_t api_fail_count;
    uint8_t *bg_rgb888;
    cs_overlay_t overlays[CS_MAX_OVERLAYS];
    size_t overlay_count;
    cs_alert_t pending_alert;
} app_shared_state_t;

static app_shared_state_t s_shared;

typedef struct {
    bool valid;
    char app_id[64];
    char etag[48];
    uint8_t *bg_rgb565;
    cs_overlay_t overlays[CS_MAX_OVERLAYS];
    size_t overlay_count;
    bool quiet_mode;
    uint32_t refresh_seconds;
    int64_t fetched_at_us;
} app_frame_cache_entry_t;

static app_frame_cache_entry_t *s_frame_cache = NULL;

static void *psram_prefer_malloc(size_t size)
{
    return heap_caps_malloc_prefer(size, 2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void *psram_prefer_calloc(size_t n, size_t size)
{
    return heap_caps_calloc_prefer(n, size, 2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void split_slot_key(const char *slot_key,
                           char *app_out, size_t app_cap,
                           char *screen_out, size_t screen_cap)
{
    if (app_out && app_cap > 0) {
        app_out[0] = '\0';
    }
    if (screen_out && screen_cap > 0) {
        screen_out[0] = '\0';
    }
    if (!slot_key || !*slot_key) {
        return;
    }
    const char *colon = strchr(slot_key, ':');
    if (colon) {
        size_t alen = (size_t)(colon - slot_key);
        if (app_out && app_cap > 0) {
            if (alen >= app_cap) {
                alen = app_cap - 1U;
            }
            memcpy(app_out, slot_key, alen);
            app_out[alen] = '\0';
        }
        if (screen_out && screen_cap > 0) {
            snprintf(screen_out, screen_cap, "%s", colon + 1);
        }
    } else if (app_out && app_cap > 0) {
        snprintf(app_out, app_cap, "%s", slot_key);
    }
}

static int find_rotary_slot_idx_locked(const char *key)
{
    if (!key || !*key) {
        return -1;
    }
    for (size_t i = 0; i < s_shared.rotary_slot_count; i++) {
        if (strcmp(s_shared.rotary_slots[i].key, key) == 0) {
            return (int)i;
        }
    }
    if (strchr(key, ':') == NULL) {
        for (size_t i = 0; i < s_shared.rotary_slot_count; i++) {
            if (strcmp(s_shared.rotary_slots[i].app_id, key) == 0) {
                return (int)i;
            }
        }
    }
    return -1;
}

static void rebuild_rotary_slots_locked(const char *preserve_key)
{
    char prev[64] = {0};
    if (preserve_key && preserve_key[0]) {
        snprintf(prev, sizeof(prev), "%s", preserve_key);
    } else if (s_shared.selected_app_idx >= 0 &&
               s_shared.selected_app_idx < (int)s_shared.rotary_slot_count) {
        snprintf(prev, sizeof(prev), "%s",
                 s_shared.rotary_slots[s_shared.selected_app_idx].key);
    }

    s_shared.rotary_slot_count = api_client_expand_rotary_slots(
        &s_shared.cfg, s_shared.rotary_slots, CS_MAX_ROTARY_SLOTS);
    if (prev[0]) {
        s_shared.selected_app_idx = find_rotary_slot_idx_locked(prev);
    } else {
        s_shared.selected_app_idx = -1;
    }
}

static void format_app_short_label(const char *app_id, char *out, size_t cap)
{
    if (!out || cap == 0) {
        return;
    }
    const char *src = "AUTO";
    if (app_id && app_id[0]) {
        char app_part[32] = {0};
        char scr_part[24] = {0};
        split_slot_key(app_id, app_part, sizeof(app_part), scr_part, sizeof(scr_part));
        if (scr_part[0]) {
            if (strcmp(scr_part, "scoreboard") == 0) {
                src = (strcmp(app_part, "cbb_scoreboard") == 0) ? "CBB SCORES" : "CFB SCORES";
            } else if (strcmp(scr_part, "countdown_time") == 0) {
                src = "COUNTDOWN";
            } else if (strcmp(scr_part, "tv_info") == 0) {
                src = "TV & WEATHER";
            } else {
                src = scr_part;
            }
        } else if (strcmp(app_part, "cfb_scoreboard") == 0) {
            src = "SCOREBOARD";
        } else if (strcmp(app_part, "cfb_countdown_clock") == 0) {
            src = "COUNTDOWN";
        } else if (strcmp(app_part, "cfb_gameday_info") == 0) {
            src = "GAMEDAY";
        } else if (strcmp(app_part, "cbb_scoreboard") == 0) {
            src = "CBB SCORES";
        } else if (strcmp(app_part, "clock") == 0) {
            src = "CLOCK";
        } else if (strcmp(app_part, "alarm") == 0) {
            src = "ALARM";
        } else {
            src = app_part;
        }
    }
    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < cap; i++) {
        out[i] = (src[i] == '_') ? ' ' : (char)toupper((unsigned char)src[i]);
    }
    out[i] = '\0';
}

static void cache_store_frame_locked(const char *app_id,
                                     const uint8_t *bg_rgb565,
                                     const char *etag,
                                     const cs_overlay_t *overlays,
                                     size_t overlay_count,
                                     bool quiet_mode,
                                     uint32_t refresh_seconds)
{
    if (!s_frame_cache || !app_id || !app_id[0] || !bg_rgb565) {
        return;
    }
    int slot = -1;
    int free_slot = -1;
    for (int i = 0; i < CS_MAX_ROTARY_SLOTS; i++) {
        if (s_frame_cache[i].valid && strcmp(s_frame_cache[i].app_id, app_id) == 0) {
            slot = i;
            break;
        }
        if (!s_frame_cache[i].valid && free_slot < 0) {
            free_slot = i;
        }
    }
    if (slot < 0) {
        slot = (free_slot >= 0) ? free_slot : 0;
    }
    app_frame_cache_entry_t *e = &s_frame_cache[slot];
    if (!e->bg_rgb565) {
        e->bg_rgb565 = psram_prefer_malloc(CS_FRAME_RGB565_BYTES);
    }
    if (!e->bg_rgb565) {
        return;
    }
    snprintf(e->app_id, sizeof(e->app_id), "%s", app_id);
    if (etag && etag[0]) {
        snprintf(e->etag, sizeof(e->etag), "%s", etag);
    } else {
        e->etag[0] = '\0';
    }
    memcpy(e->bg_rgb565, bg_rgb565, CS_FRAME_RGB565_BYTES);
    size_t cnt = (overlay_count > CS_MAX_OVERLAYS) ? CS_MAX_OVERLAYS : overlay_count;
    e->overlay_count = cnt;
    if (cnt > 0 && overlays) {
        memcpy(e->overlays, overlays, cnt * sizeof(cs_overlay_t));
    }
    e->quiet_mode = quiet_mode;
    e->refresh_seconds = refresh_seconds;
    e->fetched_at_us = esp_timer_get_time();
    e->valid = true;
}

static bool cache_get_etag_locked(const char *app_id, char *out_etag, size_t cap)
{
    if (!out_etag || cap == 0) {
        return false;
    }
    out_etag[0] = '\0';
    if (!s_frame_cache || !app_id || !app_id[0]) {
        return false;
    }
    for (int i = 0; i < CS_MAX_ROTARY_SLOTS; i++) {
        const app_frame_cache_entry_t *e = &s_frame_cache[i];
        if (e->valid && e->bg_rgb565 && e->etag[0] && strcmp(e->app_id, app_id) == 0) {
            snprintf(out_etag, cap, "%s", e->etag);
            return true;
        }
    }
    return false;
}

static void cache_touch_slot_locked(const char *app_id, uint32_t refresh_seconds)
{
    if (!s_frame_cache || !app_id || !app_id[0]) {
        return;
    }
    for (int i = 0; i < CS_MAX_ROTARY_SLOTS; i++) {
        app_frame_cache_entry_t *e = &s_frame_cache[i];
        if (e->valid && strcmp(e->app_id, app_id) == 0) {
            e->fetched_at_us = esp_timer_get_time();
            if (refresh_seconds >= 1U) {
                e->refresh_seconds = refresh_seconds;
            }
            return;
        }
    }
}

static void cache_invalidate_all_locked(void)
{
    if (!s_frame_cache) {
        return;
    }
    for (int i = 0; i < CS_MAX_ROTARY_SLOTS; i++) {
        s_frame_cache[i].valid = false;
        s_frame_cache[i].etag[0] = '\0';
        s_frame_cache[i].fetched_at_us = 0;
    }
}

static bool cache_pick_bg_refresh_app_locked(int64_t now_us, char *out_app, size_t cap)
{
    if (!s_frame_cache || !out_app || cap == 0) {
        return false;
    }
    out_app[0] = '\0';

    const char *oldest_stale_app = NULL;
    int64_t oldest_fetched_us = INT64_MAX;

    for (size_t i = 0; i < s_shared.rotary_slot_count; i++) {
        const char *slot_key = s_shared.rotary_slots[i].key;
        if (!slot_key[0] ||
            strcmp(slot_key, "alarm") == 0 ||
            strncmp(slot_key, "alarm:", 6) == 0) {
            continue;
        }
        const app_frame_cache_entry_t *match = NULL;
        for (int j = 0; j < CS_MAX_ROTARY_SLOTS; j++) {
            if (s_frame_cache[j].valid && s_frame_cache[j].bg_rgb565 &&
                strcmp(s_frame_cache[j].app_id, slot_key) == 0) {
                match = &s_frame_cache[j];
                break;
            }
        }
        if (!match) {
            snprintf(out_app, cap, "%s", slot_key);
            return true;
        }
        if (s_shared.auto_app_id[0] && strcmp(slot_key, s_shared.auto_app_id) == 0) {
            continue;
        }
        uint32_t ttl_sec = (match->refresh_seconds >= 30U)
                               ? ((match->refresh_seconds <= 300U) ? match->refresh_seconds : 300U)
                               : 60U;
        if ((now_us - match->fetched_at_us) >= (int64_t)ttl_sec * 1000000LL) {
            if (match->fetched_at_us < oldest_fetched_us) {
                oldest_fetched_us = match->fetched_at_us;
                oldest_stale_app = slot_key;
            }
        }
    }

    if (oldest_stale_app) {
        snprintf(out_app, cap, "%s", oldest_stale_app);
        return true;
    }
    return false;
}

static bool cache_apply_app_locked(const char *target_app_id)
{
    const char *lookup = (target_app_id && target_app_id[0])
                             ? target_app_id
                             : (s_shared.auto_app_id[0] ? s_shared.auto_app_id : NULL);
    if (s_frame_cache && lookup && s_shared.bg_rgb888) {
        for (int i = 0; i < CS_MAX_ROTARY_SLOTS; i++) {
            app_frame_cache_entry_t *e = &s_frame_cache[i];
            if (e->valid && e->bg_rgb565 && strcmp(e->app_id, lookup) == 0) {
                gfx_rgb565be_to_rgb888(e->bg_rgb565, CS_FRAME_PIXELS, s_shared.bg_rgb888);
                s_shared.overlay_count = e->overlay_count;
                if (e->overlay_count > 0) {
                    memcpy(s_shared.overlays, e->overlays,
                           e->overlay_count * sizeof(cs_overlay_t));
                }
                s_shared.last_quiet_mode = e->quiet_mode;
                s_shared.has_frame = true;
                snprintf(s_shared.active_app_id, sizeof(s_shared.active_app_id), "%s", e->app_id);
                s_shared.switching_pending = false;
                s_shared.switching_label[0] = '\0';
                return true;
            }
        }
    }
    s_shared.switching_pending = true;
    format_app_short_label(target_app_id, s_shared.switching_label, sizeof(s_shared.switching_label));
    return false;
}

typedef struct {
    char id[25];
    uint32_t occ_key;
} alarm_occ_record_t;

static volatile bool s_alert_playing;
static volatile bool s_alarm_ringing;
static volatile bool s_dismiss_alert;
static alarm_occ_record_t s_last_alarm_occ[CS_MAX_LOCAL_ALARMS * 2];
static size_t s_last_alarm_occ_next;
static int64_t s_suppress_server_alarm_until_us;
static volatile bool s_force_ota_check;
static volatile bool s_force_cfg_sync;
static int s_last_auto_b = -1;

#define MENU_IDLE_TIMEOUT_SEC (20U)

static void update_fast_click_locked(void)
{
    bool in_dbl_edit = (s_shared.menu_edit &&
                        (s_shared.menu_page == MENU_VOLUME ||
                         s_shared.menu_page == MENU_BRIGHTNESS));
    bool fast = s_alert_playing || (s_shared.menu_page != MENU_NONE && !in_dbl_edit);
    peripherals_set_fast_click(fast);
    peripherals_set_max_clicks(in_dbl_edit ? 2 : 3);
}

static void show_toast(const char *msg, uint32_t duration_ms)
{
    if (!msg) {
        return;
    }
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        snprintf(s_shared.toast_text, sizeof(s_shared.toast_text), "%s", msg);
        s_shared.toast_until_us = esp_timer_get_time() + (int64_t)duration_ms * 1000LL;
        xSemaphoreGive(s_state_mutex);
    }
    if (s_render_task) {
        xTaskNotifyGive(s_render_task);
    }
}

static void init_default_config(cs_device_config_t *cfg)
{
    static const char *k_default_apps[] = {
        "clock",
        "cfb_scoreboard",
    };

    memset(cfg, 0, sizeof(*cfg));
    cs_local_settings_t local;
    settings_store_get(&local);

    cfg->brightness_override = local.brightness_override;
    cfg->quiet_enabled = local.quiet_enabled;
    snprintf(cfg->device_name, sizeof(cfg->device_name), "%s", local.device_name);
    snprintf(cfg->favorite_teams, sizeof(cfg->favorite_teams), "%s", local.favorite_teams);
    snprintf(cfg->quiet_start, sizeof(cfg->quiet_start), "%s", local.quiet_start);
    snprintf(cfg->quiet_end, sizeof(cfg->quiet_end), "%s", local.quiet_end);
    snprintf(cfg->timezone, sizeof(cfg->timezone), "%s", local.timezone);

    cfg->enabled_app_count = sizeof(k_default_apps) / sizeof(k_default_apps[0]);
    for (size_t i = 0; i < cfg->enabled_app_count; i++) {
        snprintf(cfg->enabled_apps[i], sizeof(cfg->enabled_apps[i]), "%s", k_default_apps[i]);
    }
}

static void portal_get_status(char *active_app, size_t active_cap,
                              char *pinned_app, size_t pinned_cap,
                              char *apps_json_arr, size_t apps_cap,
                              uint8_t *brightness_out,
                              bool *quiet_mode_out)
{
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        snprintf(active_app, active_cap, "%s",
                 s_shared.active_app_id[0] ? s_shared.active_app_id : "clock");
        if (s_shared.manual_pinned_slot[0] != '\0') {
            snprintf(pinned_app, pinned_cap, "%s", s_shared.manual_pinned_slot);
        } else if (s_shared.selected_app_idx >= 0 &&
                   s_shared.selected_app_idx < (int)s_shared.rotary_slot_count) {
            snprintf(pinned_app, pinned_cap, "%s",
                     s_shared.rotary_slots[s_shared.selected_app_idx].key);
        } else if (pinned_cap > 0) {
            pinned_app[0] = '\0';
        }

        size_t pos = 0;
        if (apps_cap > 2) {
            apps_json_arr[pos++] = '[';
            if (s_shared.rotary_slot_count > 0) {
                for (size_t i = 0; i < s_shared.rotary_slot_count; i++) {
                    const char *k = s_shared.rotary_slots[i].key;
                    size_t need = strlen(k) + 4U;
                    if (pos + need >= apps_cap) {
                        break;
                    }
                    if (i > 0) {
                        apps_json_arr[pos++] = ',';
                    }
                    int n = snprintf(apps_json_arr + pos, apps_cap - pos, "\"%s\"", k);
                    if (n > 0) {
                        pos += (size_t)n;
                    }
                }
            } else {
                size_t count = s_shared.cfg.enabled_app_count;
                if (count > CS_MAX_APPS) {
                    count = CS_MAX_APPS;
                }
                for (size_t i = 0; i < count; i++) {
                    const char *k = s_shared.cfg.enabled_apps[i];
                    size_t need = strlen(k) + 4U;
                    if (pos + need >= apps_cap) {
                        break;
                    }
                    if (i > 0) {
                        apps_json_arr[pos++] = ',';
                    }
                    int n = snprintf(apps_json_arr + pos, apps_cap - pos, "\"%s\"", k);
                    if (n > 0) {
                        pos += (size_t)n;
                    }
                }
            }
            if (pos + 1 < apps_cap) {
                apps_json_arr[pos++] = ']';
            }
            apps_json_arr[pos] = '\0';
        }

        if (quiet_mode_out) {
            *quiet_mode_out = s_shared.last_quiet_mode;
        }
        xSemaphoreGive(s_state_mutex);
    }
    if (brightness_out) {
        *brightness_out = hub75_get_brightness();
    }
}

static bool portal_get_active_events(bool *birthdays_active, bool *holidays_active)
{
    bool known = false;
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        known = s_shared.cfg.active_events_known;
        if (birthdays_active) {
            *birthdays_active = s_shared.cfg.active_event_birthdays;
        }
        if (holidays_active) {
            *holidays_active = s_shared.cfg.active_event_holidays;
        }
        xSemaphoreGive(s_state_mutex);
    }
    return known;
}

static void portal_on_apps_metadata_synced(void)
{
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        rebuild_rotary_slots_locked(NULL);
        xSemaphoreGive(s_state_mutex);
    }
}

static void portal_get_ota_status(char *state, size_t state_cap,
                                  char *detail, size_t detail_cap)
{
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (state && state_cap > 0) {
            snprintf(state, state_cap, "%s",
                     s_shared.ota_status[0] ? s_shared.ota_status : "idle");
        }
        if (detail && detail_cap > 0) {
            snprintf(detail, detail_cap, "%s", s_shared.ota_detail);
        }
        xSemaphoreGive(s_state_mutex);
    }
}

static uint32_t portal_get_api_fail_count(void)
{
    uint32_t fails = 0;
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        fails = s_shared.api_fail_count;
        xSemaphoreGive(s_state_mutex);
    }
    return fails;
}

static void apply_brightness_mode(int brightness_override, const cs_local_settings_t *local_opt)
{
    s_last_auto_b = -1;
    if (brightness_override >= 0) {
        hub75_set_brightness((uint8_t)brightness_override);
    } else {
#if CONFIG_CS_ENABLE_LIGHT_SENSOR
        cs_local_settings_t local;
        if (local_opt) {
            local = *local_opt;
        } else {
            settings_store_get(&local);
        }
        int raw = peripherals_get_ambient_raw();
        if (raw >= 0) {
            uint8_t b = peripherals_ambient_to_brightness_range(
                raw, local.brightness_min, local.brightness_max);
            s_last_auto_b = (int)b;
            hub75_set_brightness(b);
        }
#else
        (void)local_opt;
#endif
    }
}

static void portal_on_apply_brightness(int brightness_override)
{
    apply_brightness_mode(brightness_override, NULL);
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_shared.cfg.brightness_override = brightness_override;
#if CONFIG_CS_ENABLE_LIGHT_SENSOR
        s_shared.auto_brightness = (brightness_override < 0);
#endif
        xSemaphoreGive(s_state_mutex);
    }
}

static bool step_rotary_app_locked(int step)
{
    int n = (int)s_shared.rotary_slot_count;
    s_shared.manual_pinned_slot[0] = '\0';
    if (n <= 0 || step == 0) {
        return false;
    }
    const char *auto_id = s_shared.auto_app_id[0]
                              ? s_shared.auto_app_id
                              : s_shared.active_app_id;
    int auto_idx = find_rotary_slot_idx_locked(auto_id);
    if (auto_idx >= 0) {
        /* Step relative to the currently shown Auto slot so 1 click either way always switches screens */
        int cur = (s_shared.selected_app_idx < 0) ? auto_idx : s_shared.selected_app_idx;
        int next = ((cur + step) % n + n) % n;
        s_shared.selected_app_idx = (next == auto_idx) ? -1 : next;
    } else {
        int span = n + 1;
        int pos = s_shared.selected_app_idx + 1;
        pos = ((pos + step) % span + span) % span;
        s_shared.selected_app_idx = pos - 1;
    }
    s_shared.last_rotary_us = esp_timer_get_time();
    s_shared.rotary_override_active = (s_shared.selected_app_idx >= 0);
    const char *sel = (s_shared.selected_app_idx < 0 ||
                       s_shared.selected_app_idx >= (int)s_shared.rotary_slot_count)
                          ? NULL
                          : s_shared.rotary_slots[s_shared.selected_app_idx].key;
    bool hit = cache_apply_app_locked(sel);
    ESP_LOGI(TAG, "Rotary app mode -> %s (idx=%d/%d, cached=%s)",
             sel ? sel : "auto", s_shared.selected_app_idx, n,
             hit ? "yes" : "no");
    return true;
}

static void portal_on_step_app(int delta)
{
    bool stepped = false;
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        stepped = step_rotary_app_locked(delta);
        xSemaphoreGive(s_state_mutex);
    }
    if (stepped) {
        if (s_render_task) {
            xTaskNotifyGive(s_render_task);
        }
        if (s_api_task) {
            xTaskNotifyGive(s_api_task);
        }
    }
}

static void portal_on_set_pinned_app(const char *app_id)
{
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_shared.selected_app_idx = -1;
        s_shared.manual_pinned_slot[0] = '\0';
        s_shared.rotary_override_active = false;
        const char *target = NULL;
        if (app_id && app_id[0] != '\0') {
            int idx = find_rotary_slot_idx_locked(app_id);
            if (idx >= 0) {
                s_shared.selected_app_idx = idx;
                target = s_shared.rotary_slots[idx].key;
            } else {
                snprintf(s_shared.manual_pinned_slot, sizeof(s_shared.manual_pinned_slot),
                         "%s", app_id);
                target = s_shared.manual_pinned_slot;
            }
        }
        cache_apply_app_locked(target);
        xSemaphoreGive(s_state_mutex);
    }
    if (s_api_task) {
        xTaskNotifyGive(s_api_task);
    }
    if (s_render_task) {
        xTaskNotifyGive(s_render_task);
    }
}

static void portal_on_settings_updated(const cs_local_settings_t *settings, const char *pinned_app)
{
    if (!settings) {
        return;
    }

    sntp_time_set_server(settings->ntp_server);
    sntp_time_set_timezone(settings->timezone);
    apply_brightness_mode(settings->brightness_override, settings);
#if CONFIG_CS_ENABLE_DFPLAYER
    dfplayer_set_volume(settings->df_volume);
    dfplayer_set_eq(settings->df_eq);
#endif

    bool wake_api = false;
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        bool invalidate_cache =
            (strcmp(s_shared.cfg.favorite_teams, settings->favorite_teams) != 0) ||
            (strcmp(s_shared.cfg.timezone, settings->timezone) != 0) ||
            (s_shared.cfg.quiet_enabled != settings->quiet_enabled) ||
            (strcmp(s_shared.cfg.quiet_start, settings->quiet_start) != 0) ||
            (strcmp(s_shared.cfg.quiet_end, settings->quiet_end) != 0);
        if (invalidate_cache) {
            cache_invalidate_all_locked();
            wake_api = true;
        }
        s_shared.rotary_idle_sec = settings->rotary_idle_sec;
        s_shared.cfg.brightness_override = settings->brightness_override;
        s_shared.cfg.quiet_enabled = settings->quiet_enabled;
        snprintf(s_shared.cfg.device_name, sizeof(s_shared.cfg.device_name),
                 "%s", settings->device_name);
        snprintf(s_shared.cfg.favorite_teams, sizeof(s_shared.cfg.favorite_teams),
                 "%s", settings->favorite_teams);
        snprintf(s_shared.cfg.quiet_start, sizeof(s_shared.cfg.quiet_start),
                 "%s", settings->quiet_start);
        snprintf(s_shared.cfg.quiet_end, sizeof(s_shared.cfg.quiet_end),
                 "%s", settings->quiet_end);
        snprintf(s_shared.cfg.timezone, sizeof(s_shared.cfg.timezone),
                 "%s", settings->timezone);
#if CONFIG_CS_ENABLE_LIGHT_SENSOR
        s_shared.auto_brightness = (settings->brightness_override < 0);
#endif

        if (pinned_app != NULL) {
            s_shared.selected_app_idx = -1;
            s_shared.manual_pinned_slot[0] = '\0';
            s_shared.rotary_override_active = false;
            const char *target = NULL;
            if (pinned_app[0] != '\0') {
                int idx = find_rotary_slot_idx_locked(pinned_app);
                if (idx >= 0) {
                    s_shared.selected_app_idx = idx;
                    target = s_shared.rotary_slots[idx].key;
                } else {
                    snprintf(s_shared.manual_pinned_slot, sizeof(s_shared.manual_pinned_slot),
                             "%s", pinned_app);
                    target = s_shared.manual_pinned_slot;
                }
            }
            cache_apply_app_locked(target);
            wake_api = true;
        }
        xSemaphoreGive(s_state_mutex);
    }

    if (wake_api && s_api_task) {
        xTaskNotifyGive(s_api_task);
    }
    if (s_render_task) {
        xTaskNotifyGive(s_render_task);
    }
}

static void play_df_cue(uint8_t folder, uint16_t track, bool interrupt_alert)
{
#if CONFIG_CS_ENABLE_DFPLAYER
    cs_local_settings_t local;
    settings_store_get(&local);
    if (track == 0 || local.df_volume == 0) {
        return;
    }
    if (interrupt_alert && s_alert_playing) {
        s_dismiss_alert = true;
    }
    dfplayer_set_volume(local.df_volume);
    vTaskDelay(pdMS_TO_TICKS(30));
    if (folder > 0) {
        dfplayer_play_folder_track(folder, (uint8_t)((track > 255U) ? 255U : track));
    } else {
        dfplayer_play_track(track);
    }
#else
    (void)folder;
    (void)track;
    (void)interrupt_alert;
    ESP_LOGW(TAG, "DFPlayer disabled via Kconfig");
#endif
}

static void play_manual_song(uint8_t slot)
{
    cs_local_settings_t local;
    settings_store_get(&local);
    uint8_t folder = local.fight_folder;
    uint16_t track = local.fight_track;
    const char *label = "fight song";
    if (slot == 1) {
        folder = local.victory_folder;
        track = local.victory_track;
        label = "victory song";
    } else if (slot >= 2) {
        folder = local.third_folder;
        track = local.third_track;
        label = "third song";
    }
    play_df_cue(folder, track, true);
    ESP_LOGI(TAG, "Triggered %s (folder=%u, track=%u, vol=%u)",
             label, (unsigned)folder, (unsigned)track, (unsigned)local.df_volume);
}

static bool portal_is_alarm_ringing(void)
{
    return s_alarm_ringing;
}

static void portal_on_audio_action(const char *action)
{
    if (!action) {
        return;
    }
    if (strcmp(action, "apply_volume") == 0) {
#if CONFIG_CS_ENABLE_DFPLAYER
        cs_local_settings_t local;
        settings_store_get(&local);
        dfplayer_set_volume(local.df_volume);
        if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            s_shared.menu_vol = local.df_volume;
            xSemaphoreGive(s_state_mutex);
        }
#endif
    } else if (strcmp(action, "play_fight_song") == 0) {
        play_manual_song(0);
    } else if (strcmp(action, "play_victory_song") == 0) {
        play_manual_song(1);
    } else if (strcmp(action, "play_third_song") == 0) {
        play_manual_song(2);
    } else if (strcmp(action, "test_boot_tone") == 0) {
        play_df_cue(3, 2, false);
    } else if (strcmp(action, "test_chime") == 0) {
        play_df_cue(3, 1, false);
    } else if (strcmp(action, "stop_audio") == 0) {
        if (s_alert_playing) {
            s_dismiss_alert = true;
        }
#if CONFIG_CS_ENABLE_DFPLAYER
        dfplayer_stop();
#endif
    }
}

static void portal_on_request_ota(void)
{
    s_force_ota_check = true;
    if (s_api_task) {
        xTaskNotifyGive(s_api_task);
    }
}

static void portal_on_request_refresh(void)
{
    s_force_cfg_sync = true;
    if (s_api_task) {
        xTaskNotifyGive(s_api_task);
    }
    if (s_render_task) {
        xTaskNotifyGive(s_render_task);
    }
}

static bool is_quiet_now(void)
{
    bool q_mode = false;
    bool q_enabled = false;
    char q_start[8] = "23:00";
    char q_end[8] = "07:00";

    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        q_mode = s_shared.last_quiet_mode;
        q_enabled = s_shared.cfg.quiet_enabled;
        snprintf(q_start, sizeof(q_start), "%s", s_shared.cfg.quiet_start);
        snprintf(q_end, sizeof(q_end), "%s", s_shared.cfg.quiet_end);
        xSemaphoreGive(s_state_mutex);
    }

    if (q_mode) {
        return true;
    }
    return sntp_time_in_quiet_hours(q_enabled, q_start, q_end);
}

static bool has_local_active_alarm(void)
{
    cs_alarm_schedule_t sched;
    settings_store_get_alarms(&sched);
    if (!sched.app_enabled || sched.count == 0U) {
        return false;
    }
    for (uint8_t i = 0; i < sched.count && i < CS_MAX_LOCAL_ALARMS; i++) {
        if (sched.items[i].enabled) {
            return true;
        }
    }
    return false;
}

static void draw_mini_alarm_bell(uint8_t *fb, int bx, int by)
{
    gfx_fill_rect(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT, bx + 2, by, 1, 1, 255, 205, 55);
    gfx_fill_rect(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT, bx + 1, by + 1, 3, 2, 255, 205, 55);
    gfx_fill_rect(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT, bx, by + 3, 5, 1, 255, 205, 55);
    gfx_fill_rect(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT, bx + 2, by + 4, 1, 1, 255, 245, 195);
}

static void render_local_fallback(uint8_t *fb)
{
    memset(fb, 0, CS_FRAME_RGB888_BYTES);
    if (wifi_manager_is_ap_mode() && !wifi_manager_is_connected()) {
        gfx_draw_text_aligned(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                              32, 2, "SETUP WI-FI", GFX_FONT_3X5, GFX_ALIGN_CENTER, 90, 200, 255);
        gfx_draw_text_aligned(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                              32, 12, wifi_manager_get_ap_ssid(),
                              GFX_FONT_3X5, GFX_ALIGN_CENTER, 255, 210, 70);
        gfx_draw_text_aligned(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                              32, 22, "192.168.4.1", GFX_FONT_3X5, GFX_ALIGN_CENTER, 90, 235, 130);
    } else if (sntp_time_is_synced()) {
        char tbuf[16];
        char dbuf[16];
        sntp_time_format_clock("%I:%M", tbuf, sizeof(tbuf));
        sntp_time_format_date(dbuf, sizeof(dbuf));
        gfx_draw_text_aligned(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                              32, 7, tbuf, GFX_FONT_5X7, GFX_ALIGN_CENTER, 255, 255, 255);
        gfx_draw_text_aligned(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                              32, 19, dbuf, GFX_FONT_3X5, GFX_ALIGN_CENTER, 120, 180, 255);
        if (has_local_active_alarm()) {
            draw_mini_alarm_bell(fb, 58, 25);
        }
    } else if (!wifi_manager_is_connected()) {
        gfx_draw_text_aligned(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                              32, 7, "CLEAN SLATE", GFX_FONT_3X5, GFX_ALIGN_CENTER, 180, 220, 255);
        gfx_draw_text_aligned(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                              32, 17, "WIFI...", GFX_FONT_4X6, GFX_ALIGN_CENTER, 255, 190, 60);
    } else {
        gfx_draw_text_aligned(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                              32, 7, "CLEAN SLATE", GFX_FONT_3X5, GFX_ALIGN_CENTER, 180, 220, 255);
        gfx_draw_text_aligned(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                              32, 17, "SYNCING", GFX_FONT_4X6, GFX_ALIGN_CENTER, 80, 220, 120);
    }
}

static void composite_overlays(uint8_t *fb, const cs_overlay_t *overlays, size_t count)
{
    char text[64];
    bool has_clock_time = false;
    bool has_clock_date = false;
    for (size_t i = 0; i < count; i++) {
        const cs_overlay_t *ov = &overlays[i];
        if (strcmp(ov->id, "clock_time") == 0) {
            has_clock_time = true;
        } else if (strcmp(ov->id, "clock_date") == 0) {
            has_clock_date = true;
        }
        text[0] = '\0';
        switch (ov->type) {
        case CS_OVERLAY_NTP_TIME:
            sntp_time_format_clock(ov->format, text, sizeof(text));
            break;
        case CS_OVERLAY_NTP_DATE:
            sntp_time_format_date(text, sizeof(text));
            break;
        case CS_OVERLAY_COUNTDOWN:
            if (ov->target_epoch <= 0) {
                continue;
            }
            sntp_time_format_countdown(ov->target_epoch, text, sizeof(text));
            break;
        case CS_OVERLAY_TEXT:
            snprintf(text, sizeof(text), "%s", ov->text);
            break;
        default:
            continue;
        }
        if (text[0]) {
            gfx_draw_text_aligned(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                                  ov->x, ov->y, text, ov->font, ov->align,
                                  ov->r, ov->g, ov->b);
        }
    }
    if (has_clock_time && has_local_active_alarm()) {
        draw_mini_alarm_bell(fb, 58, has_clock_date ? 18 : 24);
    }
}

#define ALERT_HOLD_FRAMES_MAX (2U)

static void play_alert_animation(cs_alert_t *alert, uint8_t *work_fb)
{
    if (!alert || !alert->valid) {
        api_client_free_alert(alert);
        return;
    }

    bool is_alarm = (strcmp(alert->event_type, "alarm") == 0);
    bool loop_alarm = is_alarm || alert->audio_repeat;
    uint32_t max_dur_s = (alert->audio_max_duration_s >= 15U && alert->audio_max_duration_s <= 900U)
                             ? alert->audio_max_duration_s
                             : 120U;
    uint32_t fallback_repeat_ms =
        (alert->audio_repeat_interval_ms >= 1500U)
            ? alert->audio_repeat_interval_ms
            : settings_store_alarm_repeat_interval_ms(alert->audio_folder, alert->audio_track);

    s_alert_playing = true;
    s_alarm_ringing = is_alarm;
    s_dismiss_alert = false;
    peripherals_set_fast_click(true);

    ESP_LOGI(TAG, "Starting alert animation: event=%s frames=%u dur=%ums strip=%uB zlib=%s repeat=%s max=%us marquee=\"%s\" free_heap=%lu",
             alert->event_type[0] ? alert->event_type : "alert",
             (unsigned)alert->frame_count,
             (unsigned)alert->frame_duration_ms,
             (unsigned)alert->strip_data_len,
             alert->strip_is_zlib ? "yes" : "no",
             loop_alarm ? "yes" : "no",
             (unsigned)(loop_alarm ? max_dur_s : 0U),
             alert->marquee_text,
             (unsigned long)esp_get_free_heap_size());

    bool audio_active = false;
    uint8_t saved_local_vol = 0;
    bool volume_overridden = false;
    int64_t alarm_start_us = esp_timer_get_time();
    int64_t last_play_us = alarm_start_us;
    int64_t replay_at_us = 0;
    bool seen_df_finish = false;

#if CONFIG_CS_ENABLE_DFPLAYER
    if (alert->has_audio && (is_alarm || !is_quiet_now())) {
        cs_local_settings_t local;
        settings_store_get(&local);
        saved_local_vol = local.df_volume;
        uint8_t vol = local.df_volume;
        if (is_alarm && alert->audio_volume > 0U) {
            vol = (alert->audio_volume > 30U) ? 30U : alert->audio_volume;
            volume_overridden = (vol != saved_local_vol);
        }
        if (vol > 0U) {
            dfplayer_flush_rx();
            dfplayer_set_volume(vol);
            vTaskDelay(pdMS_TO_TICKS(30));
            dfplayer_play_folder_track(alert->audio_folder, alert->audio_track);
            audio_active = true;
            last_play_us = esp_timer_get_time();
        }
    }
#endif

    cs_strip_stream_t *stream = api_client_strip_stream_begin(alert);
    uint8_t *scratch565 = psram_prefer_malloc(CS_FRAME_RGB565_BYTES);
    uint8_t *hold_frames[ALERT_HOLD_FRAMES_MAX] = {0};

    if (!stream || !scratch565) {
        ESP_LOGW(TAG, "Insufficient memory to play alert animation (stream=%p, scratch565=%p, free_heap=%lu, largest_block=%lu)",
                 (void *)stream, (void *)scratch565,
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        api_client_strip_stream_end(stream);
        free(scratch565);
        api_client_free_alert(alert);
        s_alarm_ringing = false;
        s_alert_playing = false;
        if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            update_fast_click_locked();
            xSemaphoreGive(s_state_mutex);
        } else {
            peripherals_set_fast_click(false);
        }
        return;
    }

    const uint16_t hold_count = (alert->frame_count < ALERT_HOLD_FRAMES_MAX)
                                    ? alert->frame_count
                                    : ALERT_HOLD_FRAMES_MAX;
    const uint16_t hold_start = (uint16_t)(alert->frame_count - hold_count);
    uint16_t held_valid = 0;

    int text_w = alert->marquee_text[0]
                     ? gfx_measure_text(alert->marquee_text, GFX_FONT_3X5)
                     : 0;
    uint32_t scroll_steps = alert->marquee_text[0]
                                ? (uint32_t)((CS_FRAME_WIDTH + text_w + 12 + 1) / 2)
                                : 0U;
    uint32_t total_steps = (alert->frame_count > scroll_steps) ? alert->frame_count : scroll_steps;
    uint32_t step_ms = (alert->frame_duration_ms >= 25U) ? alert->frame_duration_ms : 125U;

    uint32_t played_steps = 0;
    uint32_t replay_count = 0;
    const int64_t max_dur_us = (int64_t)max_dur_s * 1000000LL;
    const int64_t fallback_repeat_us = (int64_t)fallback_repeat_ms * 1000LL;

    do {
        if (!stream && alert->strip_data) {
            stream = api_client_strip_stream_begin(alert);
            if (!stream) {
                break;
            }
        }
        for (uint32_t step = 0; step < total_steps && !s_dismiss_alert; step++) {
            int64_t now_us = esp_timer_get_time();
            if (loop_alarm && (now_us - alarm_start_us) >= max_dur_us) {
                break;
            }

#if CONFIG_CS_ENABLE_DFPLAYER
            if (loop_alarm && audio_active) {
                if (dfplayer_poll_track_finished()) {
                    if (replay_at_us == 0 && (now_us - last_play_us) >= 700000LL) {
                        seen_df_finish = true;
                        replay_at_us = now_us + 300000LL;
                    }
                }
                int64_t effective_fallback_us = seen_df_finish
                                                    ? (fallback_repeat_us + 1500000LL)
                                                    : fallback_repeat_us;
                if (replay_at_us > 0 && now_us >= replay_at_us) {
                    dfplayer_flush_rx();
                    dfplayer_play_folder_track(alert->audio_folder, alert->audio_track);
                    last_play_us = now_us;
                    replay_at_us = 0;
                    replay_count++;
                } else if (replay_at_us == 0 &&
                           (now_us - last_play_us) >= effective_fallback_us) {
                    dfplayer_flush_rx();
                    dfplayer_play_folder_track(alert->audio_folder, alert->audio_track);
                    last_play_us = now_us;
                    replay_count++;
                }
            }
#endif

            bool have_frame = false;

            if (step < alert->frame_count && stream != NULL) {
                if (api_client_strip_stream_next(stream, scratch565)) {
                    have_frame = true;
                    if (step >= hold_start) {
                        uint16_t slot = (uint16_t)(step - hold_start);
                        if (!hold_frames[slot]) {
                            hold_frames[slot] = psram_prefer_malloc(CS_FRAME_RGB565_BYTES);
                        }
                        if (hold_frames[slot]) {
                            memcpy(hold_frames[slot], scratch565, CS_FRAME_RGB565_BYTES);
                            if ((uint16_t)(slot + 1U) > held_valid) {
                                held_valid = (uint16_t)(slot + 1U);
                            }
                        }
                    }
                }
                if (step + 1U >= alert->frame_count || !have_frame) {
                    api_client_strip_stream_end(stream);
                    stream = NULL;
                    if (!loop_alarm) {
                        free(alert->strip_data);
                        alert->strip_data = NULL;
                        alert->strip_data_len = 0;
                    }
                }
            } else if (held_valid > 0U) {
                uint16_t slot = (uint16_t)((step - alert->frame_count) % held_valid);
                if (hold_frames[slot]) {
                    memcpy(scratch565, hold_frames[slot], CS_FRAME_RGB565_BYTES);
                    have_frame = true;
                }
            }

            if (!have_frame) {
                break;
            }

            played_steps++;
            gfx_rgb565be_to_rgb888(scratch565, CS_FRAME_PIXELS, work_fb);

            if (alert->marquee_text[0]) {
                gfx_fill_rect(work_fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                              0, 25, CS_FRAME_WIDTH, 7, 0, 0, 0);
                int mx = CS_FRAME_WIDTH - (int)(step * 2U);
                gfx_draw_text_aligned(work_fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                                      mx, alert->marquee_y, alert->marquee_text,
                                      GFX_FONT_3X5, GFX_ALIGN_LEFT,
                                      alert->marquee_r, alert->marquee_g, alert->marquee_b);
            }

            hub75_present_rgb888(work_fb);
            vTaskDelay(pdMS_TO_TICKS(step_ms));
        }
    } while (loop_alarm && !s_dismiss_alert &&
             (esp_timer_get_time() - alarm_start_us) < max_dur_us);

#if CONFIG_CS_ENABLE_DFPLAYER
    if (loop_alarm && audio_active) {
        dfplayer_stop();
        if (volume_overridden) {
            vTaskDelay(pdMS_TO_TICKS(30));
            dfplayer_set_volume(saved_local_vol);
        }
    }
#endif

    ESP_LOGI(TAG, "Finished alert animation: event=%s steps=%lu replays=%lu dismissed=%s free_heap=%lu",
             alert->event_type[0] ? alert->event_type : "alert",
             (unsigned long)played_steps,
             (unsigned long)replay_count,
             s_dismiss_alert ? "yes" : "no",
             (unsigned long)esp_get_free_heap_size());

    api_client_strip_stream_end(stream);
    free(scratch565);
    for (size_t i = 0; i < ALERT_HOLD_FRAMES_MAX; i++) {
        free(hold_frames[i]);
    }
    api_client_free_alert(alert);
    s_dismiss_alert = false;
    s_alarm_ringing = false;
    s_alert_playing = false;
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        update_fast_click_locked();
        xSemaphoreGive(s_state_mutex);
    } else {
        peripherals_set_fast_click(false);
    }
}

static void draw_local_bell_icon(uint8_t *fb, int x0, int y0,
                                 uint8_t br, uint8_t bg, uint8_t bb,
                                 uint8_t rr, uint8_t rg, uint8_t rb,
                                 bool show_rays)
{
    gfx_fill_rect(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT, x0 + 3, y0, 1, 1, rr, rg, rb);
    gfx_fill_rect(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT, x0 + 2, y0 + 1, 3, 1, br, bg, bb);
    gfx_fill_rect(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT, x0 + 1, y0 + 2, 5, 3, br, bg, bb);
    gfx_fill_rect(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT, x0, y0 + 5, 7, 1, br, bg, bb);
    gfx_fill_rect(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT, x0 + 3, y0 + 6, 1, 1, 255, 245, 190);
    if (show_rays) {
        gfx_fill_rect(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT, x0 - 1, y0 + 1, 1, 2, rr, rg, rb);
        gfx_fill_rect(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT, x0 + 7, y0 + 1, 1, 2, rr, rg, rb);
    }
}

static void draw_local_alarm_frame(uint8_t *fb,
                                   const cs_alarm_item_t *item,
                                   bool use_24h,
                                   uint32_t frame_idx)
{
    /* Match AlarmApp 4 themes: 0=sunrise, 1=digital_pulse, 2=siren_strobe, 3=gentle_chime */
    uint8_t bgr = 24, bgg = 11, bgb = 4;
    uint8_t pr = 255, pg = 205, pb = 45;
    uint8_t rr = 255, rg = 140, rb = 45;

    if (item->theme_id == 1U) {
        bgr = 6; bgg = 8; bgb = 22;
        pr = 56; pg = 225; pb = 255;
        rr = 255; rg = 65; rb = 85;
    } else if (item->theme_id == 2U) {
        bool swap = ((frame_idx / 2U) & 1U) != 0U;
        bgr = swap ? 4 : 26; bgg = 4; bgb = swap ? 26 : 8;
        pr = swap ? 75 : 255; pg = swap ? 165 : 75; pb = swap ? 255 : 75;
        rr = swap ? 255 : 75; rg = swap ? 75 : 165; rb = swap ? 75 : 255;
    } else if (item->theme_id == 3U) {
        bgr = 6; bgg = 14; bgb = 30;
        pr = 120; pg = 210; pb = 255;
        rr = 210; rg = 240; rb = 255;
    }

    bool pulse = ((frame_idx / 3U) & 1U) != 0U;
    int wobble = ((frame_idx % 4U) == 0U) ? -1 : (((frame_idx % 4U) == 2U) ? 1 : 0);

    gfx_fill_rect(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT, 0, 0, CS_FRAME_WIDTH, CS_FRAME_HEIGHT, bgr, bgg, bgb);

    /* Pulsing top & divider accent bars */
    uint8_t ar = pulse ? pr : rr;
    uint8_t ag = pulse ? pg : rg;
    uint8_t ab = pulse ? pb : rb;
    gfx_fill_rect(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT, 0, 0, CS_FRAME_WIDTH, 1, ar, ag, ab);
    gfx_fill_rect(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT, 0, 20, CS_FRAME_WIDTH, 1, ar, ag, ab);

    /* Animated oscillating bells on left & right */
    draw_local_bell_icon(fb, 2 + wobble, 6, pr, pg, pb, rr, rg, rb, pulse);
    draw_local_bell_icon(fb, 55 - wobble, 6, pr, pg, pb, rr, rg, rb, pulse);

    /* Header + alarm label */
    gfx_draw_text_aligned(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                          32, 3, "ALARM!", GFX_FONT_3X5, GFX_ALIGN_CENTER,
                          pulse ? 255 : rr, pulse ? 255 : rg, pulse ? 255 : rb);

    char short_lbl[12] = {0};
    snprintf(short_lbl, sizeof(short_lbl), "%.11s", item->label[0] ? item->label : "WAKE UP!");
    gfx_font_id_t lbl_font = (strlen(short_lbl) <= 8U) ? GFX_FONT_4X6 : GFX_FONT_3X5;
    gfx_draw_text_aligned(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                          32, (lbl_font == GFX_FONT_4X6) ? 11 : 12,
                          short_lbl, lbl_font, GFX_ALIGN_CENTER, 255, 255, 255);

    /* Live current time strip along bottom */
    char time_txt[16] = {0};
    sntp_time_format_clock(use_24h ? "%H:%M" : "%I:%M %p", time_txt, sizeof(time_txt));
    gfx_fill_rect(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT, 0, 22, CS_FRAME_WIDTH, 10, 4, 6, 12);
    gfx_draw_text_aligned(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                          32, 24, time_txt, GFX_FONT_3X5, GFX_ALIGN_CENTER, 248, 250, 252);
}

static bool alarm_occ_already_fired(const char *alarm_id, uint32_t occ_key)
{
    if (!alarm_id || !alarm_id[0]) {
        return false;
    }
    size_t cap = sizeof(s_last_alarm_occ) / sizeof(s_last_alarm_occ[0]);
    for (size_t i = 0; i < cap; i++) {
        if (s_last_alarm_occ[i].occ_key == occ_key &&
            strcmp(s_last_alarm_occ[i].id, alarm_id) == 0) {
            return true;
        }
    }
    return false;
}

static void alarm_occ_mark_fired(const char *alarm_id, uint32_t occ_key)
{
    if (!alarm_id || !alarm_id[0]) {
        return;
    }
    size_t cap = sizeof(s_last_alarm_occ) / sizeof(s_last_alarm_occ[0]);
    for (size_t i = 0; i < cap; i++) {
        if (strcmp(s_last_alarm_occ[i].id, alarm_id) == 0) {
            s_last_alarm_occ[i].occ_key = occ_key;
            return;
        }
    }
    size_t slot = s_last_alarm_occ_next % cap;
    s_last_alarm_occ_next++;
    snprintf(s_last_alarm_occ[slot].id, sizeof(s_last_alarm_occ[slot].id), "%s", alarm_id);
    s_last_alarm_occ[slot].occ_key = occ_key;
}

static bool check_local_alarm_due(cs_alarm_item_t *out_item, cs_alarm_schedule_t *out_sched)
{
    if (!sntp_time_is_synced()) {
        return false;
    }

    cs_alarm_schedule_t sched;
    settings_store_get_alarms(&sched);
    if (!sched.app_enabled || sched.count == 0U) {
        return false;
    }
    if (!sched.allow_during_quiet_hours && is_quiet_now()) {
        return false;
    }

    time_t now_t = 0;
    time(&now_t);
    struct tm tm_now;
    localtime_r(&now_t, &tm_now);

    /* Trigger right at :00 (or within first 10s of the minute if SNTP just synced) */
    if (tm_now.tm_sec > 10) {
        return false;
    }

    uint32_t occ_key = ((uint32_t)(tm_now.tm_year + 1900) * 10000U +
                        (uint32_t)(tm_now.tm_mon + 1) * 100U +
                        (uint32_t)tm_now.tm_mday) * 1440U +
                       (uint32_t)(tm_now.tm_hour * 60 + tm_now.tm_min);
    uint8_t wday_bit = (uint8_t)(1U << (tm_now.tm_wday & 7));

    for (uint8_t i = 0; i < sched.count && i < CS_MAX_LOCAL_ALARMS; i++) {
        const cs_alarm_item_t *it = &sched.items[i];
        if (!it->enabled) {
            continue;
        }
        if (it->hour != (uint8_t)tm_now.tm_hour || it->minute != (uint8_t)tm_now.tm_min) {
            continue;
        }
        if (it->repeat && (it->days_mask & wday_bit) == 0U) {
            continue;
        }
        char dedup_id[25];
        if (it->id[0]) {
            snprintf(dedup_id, sizeof(dedup_id), "%s", it->id);
        } else {
            snprintf(dedup_id, sizeof(dedup_id), "idx_%u_%02u%02u",
                     (unsigned)i, (unsigned)it->hour, (unsigned)it->minute);
        }
        if (alarm_occ_already_fired(dedup_id, occ_key)) {
            continue;
        }
        alarm_occ_mark_fired(dedup_id, occ_key);
        if (out_item) {
            *out_item = *it;
        }
        if (out_sched) {
            *out_sched = sched;
        }
        return true;
    }
    return false;
}

static void play_local_alarm(const cs_alarm_item_t *item,
                             const cs_alarm_schedule_t *sched,
                             uint8_t *work_fb)
{
    if (!item || !sched || !work_fb) {
        return;
    }

    uint32_t max_dur_s = (uint32_t)((sched->duration_minutes >= 1U && sched->duration_minutes <= 15U)
                                        ? sched->duration_minutes
                                        : 2U) * 60U;
    uint32_t fallback_repeat_ms = settings_store_alarm_repeat_interval_ms(item->folder, item->track);

    s_alert_playing = true;
    s_alarm_ringing = true;
    s_dismiss_alert = false;
    s_suppress_server_alarm_until_us = esp_timer_get_time() + (int64_t)(max_dur_s + 60U) * 1000000LL;
    peripherals_set_fast_click(true);

    ESP_LOGI(TAG, "Local zero-delay alarm triggered: %02u:%02u label=\"%s\" sound=%u:%u repeat_ms=%lu dur=%us vol=%u",
             (unsigned)item->hour, (unsigned)item->minute,
             item->label, (unsigned)item->folder, (unsigned)item->track,
             (unsigned long)fallback_repeat_ms,
             (unsigned)max_dur_s, (unsigned)sched->volume);

    bool audio_active = false;
    uint8_t saved_local_vol = 0;
    bool volume_overridden = false;
    int64_t alarm_start_us = esp_timer_get_time();
    int64_t last_play_us = alarm_start_us;
    int64_t replay_at_us = 0;
    bool seen_df_finish = false;

#if CONFIG_CS_ENABLE_DFPLAYER
    {
        cs_local_settings_t local;
        settings_store_get(&local);
        saved_local_vol = local.df_volume;
        uint8_t vol = (sched->volume > 0U)
                          ? ((sched->volume > 30U) ? 30U : sched->volume)
                          : local.df_volume;
        volume_overridden = (vol != saved_local_vol);
        if (vol > 0U && item->folder > 0U && item->track > 0U) {
            dfplayer_flush_rx();
            dfplayer_set_volume(vol);
            vTaskDelay(pdMS_TO_TICKS(30));
            dfplayer_play_folder_track(item->folder, item->track);
            audio_active = true;
            last_play_us = esp_timer_get_time();
        }
    }
#endif

    const int64_t max_dur_us = (int64_t)max_dur_s * 1000000LL;
    const int64_t fallback_repeat_us = (int64_t)fallback_repeat_ms * 1000LL;
    uint32_t frame_idx = 0;
    uint32_t replay_count = 0;

    while (!s_dismiss_alert) {
        int64_t now_us = esp_timer_get_time();
        if ((now_us - alarm_start_us) >= max_dur_us) {
            break;
        }

#if CONFIG_CS_ENABLE_DFPLAYER
        if (audio_active) {
            if (dfplayer_poll_track_finished()) {
                if (replay_at_us == 0 && (now_us - last_play_us) >= 700000LL) {
                    seen_df_finish = true;
                    replay_at_us = now_us + 300000LL;
                }
            }
            int64_t effective_fallback_us = seen_df_finish
                                                ? (fallback_repeat_us + 1500000LL)
                                                : fallback_repeat_us;
            if (replay_at_us > 0 && now_us >= replay_at_us) {
                dfplayer_flush_rx();
                dfplayer_play_folder_track(item->folder, item->track);
                last_play_us = now_us;
                replay_at_us = 0;
                replay_count++;
            } else if (replay_at_us == 0 &&
                       (now_us - last_play_us) >= effective_fallback_us) {
                dfplayer_flush_rx();
                dfplayer_play_folder_track(item->folder, item->track);
                last_play_us = now_us;
                replay_count++;
            }
        }
#endif

        draw_local_alarm_frame(work_fb, item, sched->use_24h, frame_idx++);
        hub75_present_rgb888(work_fb);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

#if CONFIG_CS_ENABLE_DFPLAYER
    if (audio_active) {
        dfplayer_stop();
        if (volume_overridden) {
            vTaskDelay(pdMS_TO_TICKS(30));
            dfplayer_set_volume(saved_local_vol);
        }
    }
#endif

    ESP_LOGI(TAG, "Local alarm finished: label=\"%s\" repeat=%s frames=%lu replays=%lu dismissed=%s",
             item->label, item->repeat ? "yes" : "no",
             (unsigned long)frame_idx, (unsigned long)replay_count,
             s_dismiss_alert ? "yes" : "no");

    if (!item->repeat && item->id[0]) {
        settings_store_consume_onetime_alarm(item->id);
        if (s_api_task) {
            xTaskNotifyGive(s_api_task);
        }
    }

    s_dismiss_alert = false;
    s_alarm_ringing = false;
    s_alert_playing = false;
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        update_fast_click_locked();
        xSemaphoreGive(s_state_mutex);
    } else {
        peripherals_set_fast_click(false);
    }
}

static void render_menu_overlay(uint8_t *fb, local_menu_page_t page,
                                bool edit, uint8_t vol, int bright, int tz_idx)
{
    gfx_fill_rect(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT, 0, 0, CS_FRAME_WIDTH, CS_FRAME_HEIGHT, 12, 20, 36);
    if (edit) {
        gfx_fill_rect(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT, 0, 0, CS_FRAME_WIDTH, 1, 255, 220, 80);
        gfx_fill_rect(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT, 0, 31, CS_FRAME_WIDTH, 1, 255, 220, 80);
        gfx_fill_rect(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT, 0, 0, 1, CS_FRAME_HEIGHT, 255, 220, 80);
        gfx_fill_rect(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT, 63, 0, 1, CS_FRAME_HEIGHT, 255, 220, 80);
    }

    char line[32];
    switch (page) {
    case MENU_VOLUME: {
        snprintf(line, sizeof(line), "VOL %u", (unsigned)vol);
        gfx_draw_text_aligned(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                              32, 3, line, GFX_FONT_5X7, GFX_ALIGN_CENTER, 255, 255, 255);
        gfx_fill_rect(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT, 2, 16, 60, 8, 45, 58, 78);
        if (vol > 0) {
            int bar_w = (int)((vol > 30U ? 30U : vol) * 2U);
            gfx_fill_rect(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT, 2, 16, bar_w, 8,
                          edit ? 255 : 90, edit ? 210 : 200, edit ? 70 : 255);
        }
        gfx_draw_text_aligned(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                              32, 26, edit ? "TURN / CLICK" : "CLICK TO EDIT",
                              GFX_FONT_3X5, GFX_ALIGN_CENTER, 175, 195, 225);
        break;
    }
    case MENU_BRIGHTNESS: {
        if (bright < 0) {
            snprintf(line, sizeof(line), "BRT AUTO");
        } else {
            snprintf(line, sizeof(line), "BRT %d", bright);
        }
        gfx_draw_text_aligned(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                              32, 3, line, GFX_FONT_5X7, GFX_ALIGN_CENTER, 255, 255, 255);
        gfx_fill_rect(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT, 2, 16, 60, 8, 45, 58, 78);
        uint8_t eff_b = (bright < 0) ? hub75_get_brightness() : (uint8_t)bright;
        int bar_w = (int)(((uint32_t)eff_b * 60U + 127U) / 255U);
        if (bar_w > 0) {
            gfx_fill_rect(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT, 2, 16, bar_w, 8,
                          edit ? 255 : (bright < 0 ? 110 : 90),
                          edit ? 210 : (bright < 0 ? 235 : 200),
                          edit ? 70  : (bright < 0 ? 150 : 255));
        }
        gfx_draw_text_aligned(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                              32, 26, edit ? "TURN / CLICK" : "CLICK TO EDIT",
                              GFX_FONT_3X5, GFX_ALIGN_CENTER, 175, 195, 225);
        break;
    }
    case MENU_WIFI: {
        gfx_draw_text_aligned(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                              32, 3, "WIFI", GFX_FONT_5X7, GFX_ALIGN_CENTER, 255, 255, 255);
        if (wifi_manager_is_ap_mode() && !wifi_manager_is_connected()) {
            gfx_draw_text_aligned(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                                  32, 13, wifi_manager_get_ap_ssid(),
                                  GFX_FONT_3X5, GFX_ALIGN_CENTER, 255, 210, 70);
        } else {
            char ip[20] = "";
            wifi_manager_get_ip_str(ip, sizeof(ip));
            gfx_draw_text_aligned(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                                  32, 13, ip[0] ? ip : "OFFLINE",
                                  GFX_FONT_3X5, GFX_ALIGN_CENTER, 110, 225, 145);
        }
        gfx_draw_text_aligned(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                              32, 23, edit ? "HOLD:SETUP AP" : "CLICK TO EDIT",
                              GFX_FONT_3X5, GFX_ALIGN_CENTER, 175, 195, 225);
        break;
    }
    case MENU_TIMEZONE: {
        int idx = tz_idx;
        int cnt = (int)(sizeof(k_menu_tz) / sizeof(k_menu_tz[0]));
        if (idx < 0 || idx >= cnt) {
            idx = 1;
        }
        char tbuf[16];
        sntp_time_format_clock("%I:%M", tbuf, sizeof(tbuf));
        gfx_draw_text_aligned(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                              32, 3, "TIMEZONE", GFX_FONT_5X7, GFX_ALIGN_CENTER, 255, 255, 255);
        gfx_draw_text_aligned(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                              32, 13, k_menu_tz[idx].label,
                              GFX_FONT_5X7, GFX_ALIGN_CENTER, 255, 215, 80);
        gfx_draw_text_aligned(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                              32, 24, tbuf, GFX_FONT_3X5, GFX_ALIGN_CENTER, 175, 195, 225);
        break;
    }
    case MENU_FACTORY_RESET:
        gfx_draw_text_aligned(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                              32, 3, "FACTORY RST", GFX_FONT_4X6, GFX_ALIGN_CENTER, 255, 120, 120);
        gfx_draw_text_aligned(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                              32, 14, edit ? "HOLD 1S TO" : "CLEAR ALL",
                              GFX_FONT_3X5, GFX_ALIGN_CENTER, 255, 255, 255);
        gfx_draw_text_aligned(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                              32, 23, edit ? "ERASE & BOOT" : "CLICK TO EDIT",
                              GFX_FONT_3X5, GFX_ALIGN_CENTER, edit ? 255 : 175, edit ? 190 : 195, edit ? 80 : 225);
        break;
    default:
        break;
    }
}

static void render_switching_overlay(uint8_t *fb, const char *label)
{
    gfx_fill_rect(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT, 0, 0, CS_FRAME_WIDTH, CS_FRAME_HEIGHT, 10, 16, 30);
    gfx_fill_rect(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT, 0, 0, CS_FRAME_WIDTH, 2, 70, 185, 255);
    gfx_draw_text_aligned(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                          32, 9, (label && label[0]) ? label : "AUTO",
                          GFX_FONT_4X6, GFX_ALIGN_CENTER, 255, 255, 255);
    gfx_draw_text_aligned(fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                          32, 20, "LOADING...",
                          GFX_FONT_3X5, GFX_ALIGN_CENTER, 120, 215, 255);
}

static void render_task(void *arg)
{
    (void)arg;
    uint8_t *work_fb = psram_prefer_malloc(CS_FRAME_RGB888_BYTES);
    uint8_t *last_fb = psram_prefer_malloc(CS_FRAME_RGB888_BYTES);
    if (!work_fb || !last_fb) {
        ESP_LOGE(TAG, "Failed to allocate render framebuffers");
        vTaskDelete(NULL);
        return;
    }

    bool last_valid = false;
    cs_overlay_t overlays_copy[CS_MAX_OVERLAYS];

    while (true) {
        cs_alert_t alert = {0};
        bool has_frame = false;
        bool switching_pending = false;
        char switching_label[24] = "";
        size_t overlay_count = 0;
        local_menu_page_t menu_page = MENU_NONE;
        bool menu_edit = false;
        uint8_t menu_vol = 18;
        int menu_bright = -1;
        int menu_tz_idx = 1;
        char toast[24] = "";
        int64_t toast_until = 0;

        if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (s_shared.pending_alert.valid) {
                alert = s_shared.pending_alert;
                memset(&s_shared.pending_alert, 0, sizeof(s_shared.pending_alert));
            }
            menu_page = s_shared.menu_page;
            menu_edit = s_shared.menu_edit;
            menu_vol = s_shared.menu_vol;
            menu_bright = s_shared.menu_bright;
            menu_tz_idx = s_shared.menu_tz_idx;
            switching_pending = s_shared.switching_pending;
            if (switching_pending) {
                snprintf(switching_label, sizeof(switching_label), "%s", s_shared.switching_label);
            }
            if (s_shared.toast_text[0] && s_shared.toast_until_us > esp_timer_get_time()) {
                snprintf(toast, sizeof(toast), "%s", s_shared.toast_text);
                toast_until = s_shared.toast_until_us;
            } else {
                s_shared.toast_text[0] = '\0';
            }
            has_frame = s_shared.has_frame;
            if (has_frame && menu_page == MENU_NONE && !switching_pending && s_shared.bg_rgb888 != NULL) {
                memcpy(work_fb, s_shared.bg_rgb888, CS_FRAME_RGB888_BYTES);
                overlay_count = s_shared.overlay_count;
                if (overlay_count > CS_MAX_OVERLAYS) {
                    overlay_count = CS_MAX_OVERLAYS;
                }
                if (overlay_count > 0) {
                    memcpy(overlays_copy, s_shared.overlays,
                           overlay_count * sizeof(cs_overlay_t));
                }
            }
            xSemaphoreGive(s_state_mutex);
        }

        /* Check local SNTP alarms every 100ms for exact :00 zero-delay triggering */
        {
            cs_alarm_item_t due_item;
            cs_alarm_schedule_t due_sched;
            if (check_local_alarm_due(&due_item, &due_sched)) {
                api_client_free_alert(&alert);
                play_local_alarm(&due_item, &due_sched, work_fb);
                last_valid = false;
                continue;
            }
        }

        if (alert.valid) {
            if (strcmp(alert.event_type, "alarm") == 0 &&
                esp_timer_get_time() < s_suppress_server_alarm_until_us) {
                api_client_free_alert(&alert);
            } else {
                play_alert_animation(&alert, work_fb);
                last_valid = false;
                continue;
            }
        }

        if (menu_page != MENU_NONE) {
            render_menu_overlay(work_fb, menu_page, menu_edit, menu_vol, menu_bright, menu_tz_idx);
        } else if (switching_pending) {
            render_switching_overlay(work_fb, switching_label);
        } else if (!has_frame || (wifi_manager_is_ap_mode() && !wifi_manager_is_connected())) {
            render_local_fallback(work_fb);
        } else {
            composite_overlays(work_fb, overlays_copy, overlay_count);
        }

        if (toast[0] && toast_until > esp_timer_get_time()) {
            gfx_fill_rect(work_fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT, 4, 21, 56, 10, 8, 14, 26);
            gfx_draw_text_aligned(work_fb, CS_FRAME_WIDTH, CS_FRAME_HEIGHT,
                                  32, 23, toast, GFX_FONT_3X5, GFX_ALIGN_CENTER, 110, 235, 165);
        }

        if (!last_valid || memcmp(work_fb, last_fb, CS_FRAME_RGB888_BYTES) != 0) {
            memcpy(last_fb, work_fb, CS_FRAME_RGB888_BYTES);
            last_valid = true;
            hub75_present_rgb888(work_fb);
        }

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }
}

static int find_menu_tz_index(const char *tz)
{
    int cnt = (int)(sizeof(k_menu_tz) / sizeof(k_menu_tz[0]));
    if (tz && *tz) {
        for (int i = 0; i < cnt; i++) {
            if (strcmp(k_menu_tz[i].iana, tz) == 0) {
                return i;
            }
        }
    }
    return 1; /* Default CST / America/Chicago */
}

static void save_menu_changes(local_menu_page_t page, uint8_t vol, int bright, int tz_idx)
{
    cs_local_settings_t cfg;
    settings_store_get(&cfg);
    bool changed = false;

    if (page == MENU_VOLUME && cfg.df_volume != vol) {
        cfg.df_volume = (vol <= 30U) ? vol : 30U;
        changed = true;
    } else if (page == MENU_BRIGHTNESS && cfg.brightness_override != bright) {
        cfg.brightness_override = (bright >= 0 && bright <= 255) ? bright : -1;
        cfg.cloud_sync_pending = true;
        changed = true;
    } else if (page == MENU_TIMEZONE) {
        int cnt = (int)(sizeof(k_menu_tz) / sizeof(k_menu_tz[0]));
        if (tz_idx >= 0 && tz_idx < cnt && strcmp(cfg.timezone, k_menu_tz[tz_idx].iana) != 0) {
            snprintf(cfg.timezone, sizeof(cfg.timezone), "%s", k_menu_tz[tz_idx].iana);
            cfg.cloud_sync_pending = true;
            changed = true;
        }
    }
    if (changed) {
        settings_store_save(&cfg);
        if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            snprintf(s_shared.cfg.timezone, sizeof(s_shared.cfg.timezone), "%s", cfg.timezone);
            s_shared.cfg.brightness_override = cfg.brightness_override;
#if CONFIG_CS_ENABLE_LIGHT_SENSOR
            s_shared.auto_brightness = (cfg.brightness_override < 0);
#endif
            xSemaphoreGive(s_state_mutex);
        }
    }
}

static void input_task(void *arg)
{
    QueueHandle_t q = (QueueHandle_t)arg;
    cs_input_event_t ev;
    int64_t last_lux_us = -500000LL;

    while (true) {
        if (q && xQueueReceive(q, &ev, pdMS_TO_TICKS(100)) == pdTRUE) {
            bool wake_api = false;
            bool wake_render = false;
            bool do_enter_setup = false;
            bool do_factory_reset = false;
            bool need_save_menu = false;
            bool in_menu = false;
            local_menu_page_t save_page = MENU_NONE;
            uint8_t save_vol = 18;
            int save_bright = -1;
            int save_tz_idx = 1;
            int apply_vol = -1;
            int apply_bright = -2;
            const char *apply_tz = NULL;
            const char *toast_msg = NULL;

            cs_local_settings_t cur_cfg = {0};
            bool have_cur_cfg = false;

            if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                bool need_cfg = (s_shared.menu_page != MENU_NONE) ||
                                (ev.type == CS_INPUT_BTN_SHORT ||
                                 ev.type == CS_INPUT_BTN_DOUBLE ||
                                 ev.type == CS_INPUT_BTN_LONG);
                if (need_cfg) {
                    xSemaphoreGive(s_state_mutex);
                    settings_store_get(&cur_cfg);
                    have_cur_cfg = true;
                    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
                        continue;
                    }
                }
                (void)have_cur_cfg;

                int step = (ev.delta != 0) ? (int)ev.delta
                                           : ((ev.type == CS_INPUT_ENC_CW) ? 1 : -1);

                /* Coalesce any additional buffered rotary ticks sitting in the queue */
                if (ev.type == CS_INPUT_ENC_CW || ev.type == CS_INPUT_ENC_CCW) {
                    cs_input_event_t extra;
                    while (xQueueReceive(q, &extra, 0) == pdTRUE) {
                        if (extra.type == CS_INPUT_ENC_CW || extra.type == CS_INPUT_ENC_CCW) {
                            step += (extra.delta != 0) ? (int)extra.delta
                                                       : ((extra.type == CS_INPUT_ENC_CW) ? 1 : -1);
                        } else {
                            xQueueSendToFront(q, &extra, 0);
                            break;
                        }
                    }
                }

                if (s_shared.menu_page != MENU_NONE) {
                    in_menu = true;
                    s_shared.last_menu_us = esp_timer_get_time();
                    wake_render = true;

                    if (s_shared.menu_edit) {
                        if (ev.type == CS_INPUT_ENC_CW || ev.type == CS_INPUT_ENC_CCW) {
                            if (s_shared.menu_page == MENU_VOLUME) {
                                int v = (int)s_shared.menu_vol + step;
                                if (v < 0) v = 0;
                                if (v > 30) v = 30;
                                s_shared.menu_vol = (uint8_t)v;
                                apply_vol = v;
                            } else if (s_shared.menu_page == MENU_BRIGHTNESS) {
                                int b = s_shared.menu_bright;
                                if (b < 0) {
                                    b = (step > 0) ? 16 : -1;
                                } else {
                                    b += step * 8;
                                    if (b < 1) {
                                        b = -1; /* Step below 1 returns to AUTO */
                                    } else if (b > 255) {
                                        b = 255;
                                    }
                                }
                                s_shared.menu_bright = b;
                                apply_bright = b;
                            } else if (s_shared.menu_page == MENU_TIMEZONE) {
                                int cnt = (int)(sizeof(k_menu_tz) / sizeof(k_menu_tz[0]));
                                s_shared.menu_tz_idx = ((s_shared.menu_tz_idx + step) % cnt + cnt) % cnt;
                                apply_tz = k_menu_tz[s_shared.menu_tz_idx].iana;
                            }
                        } else if (ev.type == CS_INPUT_BTN_DOUBLE && s_shared.menu_page == MENU_VOLUME) {
                            if (s_shared.menu_vol == 30) {
                                s_shared.menu_vol = 0;
                            } else if (s_shared.menu_vol < 15) {
                                s_shared.menu_vol = 15;
                            } else {
                                s_shared.menu_vol = 30;
                            }
                            apply_vol = (int)s_shared.menu_vol;
                        } else if (ev.type == CS_INPUT_BTN_DOUBLE && s_shared.menu_page == MENU_BRIGHTNESS) {
                            s_shared.menu_bright = (s_shared.menu_bright < 0) ? 128 : -1;
                            apply_bright = s_shared.menu_bright;
                        } else if (ev.type == CS_INPUT_BTN_SHORT) {
                            s_shared.menu_edit = false;
                            need_save_menu = true;
                            save_page = s_shared.menu_page;
                            save_vol = s_shared.menu_vol;
                            save_bright = s_shared.menu_bright;
                            save_tz_idx = s_shared.menu_tz_idx;
                            if (s_shared.menu_page == MENU_VOLUME ||
                                s_shared.menu_page == MENU_BRIGHTNESS ||
                                s_shared.menu_page == MENU_TIMEZONE) {
                                toast_msg = "SAVED";
                            }
                        } else if (ev.type == CS_INPUT_BTN_LONG) {
                            if (s_shared.menu_page == MENU_WIFI) {
                                s_shared.menu_page = MENU_NONE;
                                s_shared.menu_edit = false;
                                do_enter_setup = true;
                                toast_msg = "SETUP AP";
                            } else if (s_shared.menu_page == MENU_FACTORY_RESET) {
                                s_shared.menu_page = MENU_NONE;
                                s_shared.menu_edit = false;
                                do_factory_reset = true;
                            } else {
                                /* Cancel unsaved edit changes */
                                s_shared.menu_edit = false;
                                if (s_shared.menu_page == MENU_VOLUME) {
                                    s_shared.menu_vol = cur_cfg.df_volume;
                                    apply_vol = (int)cur_cfg.df_volume;
                                } else if (s_shared.menu_page == MENU_BRIGHTNESS) {
                                    s_shared.menu_bright = cur_cfg.brightness_override;
                                    apply_bright = cur_cfg.brightness_override;
                                } else if (s_shared.menu_page == MENU_TIMEZONE) {
                                    s_shared.menu_tz_idx = find_menu_tz_index(cur_cfg.timezone);
                                    apply_tz = cur_cfg.timezone;
                                }
                                toast_msg = "CANCELED";
                            }
                        }
                    } else {
                        if (ev.type == CS_INPUT_ENC_CW || ev.type == CS_INPUT_ENC_CCW) {
                            int span = (int)MENU_COUNT - 1;
                            int idx = ((int)s_shared.menu_page - 1 + step);
                            idx = ((idx % span) + span) % span;
                            s_shared.menu_page = (local_menu_page_t)(idx + 1);
                            if (s_shared.menu_page == MENU_VOLUME) {
                                s_shared.menu_vol = cur_cfg.df_volume;
                            } else if (s_shared.menu_page == MENU_BRIGHTNESS) {
                                s_shared.menu_bright = cur_cfg.brightness_override;
                            } else if (s_shared.menu_page == MENU_TIMEZONE) {
                                s_shared.menu_tz_idx = find_menu_tz_index(cur_cfg.timezone);
                            }
                        } else if (ev.type == CS_INPUT_BTN_SHORT) {
                            s_shared.menu_edit = true;
                            if (s_shared.menu_page == MENU_VOLUME) {
                                s_shared.menu_vol = cur_cfg.df_volume;
                            } else if (s_shared.menu_page == MENU_BRIGHTNESS) {
                                s_shared.menu_bright = cur_cfg.brightness_override;
                            } else if (s_shared.menu_page == MENU_TIMEZONE) {
                                s_shared.menu_tz_idx = find_menu_tz_index(cur_cfg.timezone);
                            }
                        } else if (ev.type == CS_INPUT_BTN_LONG) {
                            s_shared.menu_page = MENU_NONE;
                            s_shared.menu_edit = false;
                            s_shared.selected_app_idx = -1;
                            s_shared.rotary_override_active = false;
                            wake_api = true;
                        }
                    }
                } else {
                    if (ev.type == CS_INPUT_ENC_CW || ev.type == CS_INPUT_ENC_CCW) {
                        if (step_rotary_app_locked(step)) {
                            wake_render = true;
                            wake_api = true;
                        }
                    } else if (ev.type == CS_INPUT_BTN_LONG) {
                        in_menu = true;
                        s_shared.menu_page = MENU_VOLUME;
                        s_shared.menu_edit = false;
                        s_shared.menu_vol = cur_cfg.df_volume;
                        s_shared.menu_bright = cur_cfg.brightness_override;
                        s_shared.menu_tz_idx = find_menu_tz_index(cur_cfg.timezone);
                        s_shared.last_menu_us = esp_timer_get_time();
                        wake_render = true;
                        ESP_LOGI(TAG, "Button long press: opened on-device Settings menu (VOLUME)");
                    }
                }
                update_fast_click_locked();
                xSemaphoreGive(s_state_mutex);
            }

            if (!in_menu) {
                if (s_alert_playing &&
                    (ev.type == CS_INPUT_BTN_SHORT ||
                     ev.type == CS_INPUT_BTN_DOUBLE ||
                     ev.type == CS_INPUT_BTN_TRIPLE)) {
                    s_dismiss_alert = true;
#if CONFIG_CS_ENABLE_DFPLAYER
                    dfplayer_stop();
#endif
                    ESP_LOGI(TAG, "Alert dismissed via button press");
                } else if (ev.type == CS_INPUT_BTN_SHORT) {
                    play_manual_song(0);
                } else if (ev.type == CS_INPUT_BTN_DOUBLE) {
                    play_manual_song(1);
                } else if (ev.type == CS_INPUT_BTN_TRIPLE) {
                    play_manual_song(2);
                }
            }

            if (apply_vol >= 0) {
#if CONFIG_CS_ENABLE_DFPLAYER
                dfplayer_set_volume((uint8_t)apply_vol);
#endif
                ESP_LOGI(TAG, "Menu volume -> %d", apply_vol);
            }
            if (apply_bright >= -1) {
                apply_brightness_mode(apply_bright, &cur_cfg);
                if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                    s_shared.cfg.brightness_override = apply_bright;
#if CONFIG_CS_ENABLE_LIGHT_SENSOR
                    s_shared.auto_brightness = (apply_bright < 0);
#endif
                    xSemaphoreGive(s_state_mutex);
                }
            }
            if (apply_tz) {
                sntp_time_set_timezone(apply_tz);
            }
            if (need_save_menu) {
                save_menu_changes(save_page, save_vol, save_bright, save_tz_idx);
            }
            if (toast_msg) {
                show_toast(toast_msg, 1200);
            }
            if (do_enter_setup) {
                ESP_LOGI(TAG, "On-device menu: starting Captive Portal AP setup");
                wifi_manager_enter_setup_mode();
            }
            if (do_factory_reset) {
                ESP_LOGW(TAG, "On-device menu: Factory Reset triggered");
                hub75_show_status_screen("FACTORY RST", "ERASING...", 255, 110, 110);
                settings_store_factory_reset();
                esp_wifi_restore();
                vTaskDelay(pdMS_TO_TICKS(600));
                esp_restart();
            }
            if (wake_render && s_render_task) {
                xTaskNotifyGive(s_render_task);
            }
            if (wake_api && s_api_task) {
                xTaskNotifyGive(s_api_task);
            }
        }

        int64_t now_us = esp_timer_get_time();

        bool exit_menu = false;
        local_menu_page_t exit_page = MENU_NONE;
        uint8_t exit_vol = 18;
        int exit_bright = -1;
        int exit_tz_idx = 1;
        bool return_auto = false;
        uint16_t rotary_idle_sec = 30U;

        if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            rotary_idle_sec = s_shared.rotary_idle_sec;
            if (s_shared.menu_page != MENU_NONE &&
                (now_us - s_shared.last_menu_us) >= (int64_t)MENU_IDLE_TIMEOUT_SEC * 1000000LL) {
                exit_page = s_shared.menu_page;
                exit_vol = s_shared.menu_vol;
                exit_bright = s_shared.menu_bright;
                exit_tz_idx = s_shared.menu_tz_idx;
                s_shared.menu_page = MENU_NONE;
                s_shared.menu_edit = false;
                s_shared.selected_app_idx = -1;
                s_shared.manual_pinned_slot[0] = '\0';
                s_shared.rotary_override_active = false;
                cache_apply_app_locked(NULL);
                update_fast_click_locked();
                exit_menu = true;
            } else if (rotary_idle_sec > 0 &&
                       s_shared.rotary_override_active && s_shared.selected_app_idx >= 0 &&
                       (now_us - s_shared.last_rotary_us) >= (int64_t)rotary_idle_sec * 1000000LL) {
                s_shared.selected_app_idx = -1;
                s_shared.manual_pinned_slot[0] = '\0';
                s_shared.rotary_override_active = false;
                cache_apply_app_locked(NULL);
                return_auto = true;
            }
            xSemaphoreGive(s_state_mutex);
        }

        if (exit_menu) {
            save_menu_changes(exit_page, exit_vol, exit_bright, exit_tz_idx);
            ESP_LOGI(TAG, "Menu idle timeout (%us): returning to Auto schedule", (unsigned)MENU_IDLE_TIMEOUT_SEC);
            if (s_render_task) {
                xTaskNotifyGive(s_render_task);
            }
            if (s_api_task) {
                xTaskNotifyGive(s_api_task);
            }
        } else if (return_auto) {
            ESP_LOGI(TAG, "Rotary idle timeout (%us): returning to Auto schedule",
                     (unsigned)rotary_idle_sec);
            if (s_render_task) {
                xTaskNotifyGive(s_render_task);
            }
            if (s_api_task) {
                xTaskNotifyGive(s_api_task);
            }
        }

        if (now_us - last_lux_us >= 500000LL) {
            last_lux_us = now_us;
            int override = -1;
            bool auto_b = false;

            if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                override = s_shared.cfg.brightness_override;
                auto_b = s_shared.auto_brightness;
                xSemaphoreGive(s_state_mutex);
            }

            if (override >= 0) {
                s_last_auto_b = -1;
                hub75_set_brightness((uint8_t)override);
            } else if (auto_b) {
                cs_local_settings_t b_cfg;
                settings_store_get(&b_cfg);
                int raw = peripherals_get_ambient_raw();
                if (raw >= 0) {
                    uint8_t target = peripherals_ambient_to_brightness_range(
                        raw, b_cfg.brightness_min, b_cfg.brightness_max);
                    int diff = (s_last_auto_b < 0) ? 255 : abs((int)target - s_last_auto_b);
                    bool at_edge = (target == b_cfg.brightness_min || target == b_cfg.brightness_max) &&
                                   ((int)target != s_last_auto_b);
                    if (diff >= 3 || at_edge) {
                        s_last_auto_b = (int)target;
                        hub75_set_brightness(target);
                    }
                }
            }
        }
    }
}

static void maybe_play_hourly_chime(int *last_chime_hour)
{
#if CONFIG_CS_ENABLE_DFPLAYER
    cs_local_settings_t local;
    settings_store_get(&local);
    if (!local.hourly_chime || !sntp_time_is_synced() || s_alert_playing || is_quiet_now()) {
        return;
    }
    time_t now = time(NULL);
    struct tm tm_info = {0};
    localtime_r(&now, &tm_info);
    if (tm_info.tm_min == 0 && tm_info.tm_hour != *last_chime_hour) {
        *last_chime_hour = tm_info.tm_hour;
        dfplayer_set_volume(local.df_volume);
        vTaskDelay(pdMS_TO_TICKS(30));
        dfplayer_play_folder_track(3, 1);
    }
#else
    (void)last_chime_hour;
#endif
}

static void api_task(void *arg)
{
    (void)arg;
    bool sntp_started = false;
    bool boot_tone_done = api_client_consume_ota_reboot_flag();
    int64_t online_since_us = 0;
    int64_t last_cfg_us = -((int64_t)CONFIG_CS_API_CONFIG_SYNC_SEC * 1000000LL);
    int64_t last_ota_us = -((int64_t)CONFIG_CS_OTA_CHECK_INTERVAL_SEC * 1000000LL);
    uint32_t fail_count = 0;
    int last_chime_hour = -1;

    while (true) {
        if (!wifi_manager_is_connected()) {
            hub75_set_status_indicator(wifi_manager_is_ap_mode()
                                           ? HUB75_STATUS_NONE
                                           : HUB75_STATUS_CONNECTING);
            wifi_manager_wait_connected(5000);
            if (s_render_task) {
                xTaskNotifyGive(s_render_task);
            }
            maybe_play_hourly_chime(&last_chime_hour);
            continue;
        }

        if (!sntp_started) {
            cs_local_settings_t local;
            settings_store_get(&local);
            sntp_time_set_server(local.ntp_server);
            sntp_time_set_timezone(local.timezone);
            sntp_time_init();
            sntp_started = true;
            online_since_us = esp_timer_get_time();
            char ip[32];
            wifi_manager_get_ip_str(ip, sizeof(ip));
            ESP_LOGI(TAG, "Online! Open local config at http://%s/ or http://%s.local/",
                     ip, wifi_manager_get_device_id());
        }

        int64_t now_us = esp_timer_get_time();

#if CONFIG_CS_ENABLE_DFPLAYER
        if (!boot_tone_done &&
            (sntp_time_is_synced() || (online_since_us > 0 && (now_us - online_since_us) >= 15000000LL))) {
            boot_tone_done = true;
            cs_local_settings_t local;
            settings_store_get(&local);
            if (local.boot_tone && local.df_volume > 0 && !is_quiet_now()) {
                dfplayer_set_volume(local.df_volume);
                vTaskDelay(pdMS_TO_TICKS(30));
                dfplayer_play_folder_track(3, 2);
            }
        }
#else
        (void)boot_tone_done;
        (void)online_since_us;
#endif

        cs_local_settings_t local;
        settings_store_get(&local);
        if (local.cloud_sync_pending) {
            if (api_client_post_config(&local, NULL, NULL) == ESP_OK) {
                local.cloud_sync_pending = false;
                settings_store_save(&local);
                ESP_LOGI(TAG, "Pushed pending local setup (team=%s) to Clean Slate server",
                         local.favorite_teams);
                last_cfg_us = now_us;
            }
        }

        /* Sync any locally auto-deleted one-time alarms back to the cloud server */
        {
            cs_alarm_schedule_t pending_alarms;
            settings_store_get_alarms(&pending_alarms);
            if (pending_alarms.pending_delete_count > 0U) {
                for (uint8_t i = 0; i < pending_alarms.pending_delete_count && i < CS_MAX_LOCAL_ALARMS; i++) {
                    if (pending_alarms.pending_delete_ids[i][0] &&
                        api_client_delete_alarm(pending_alarms.pending_delete_ids[i]) == ESP_OK) {
                        settings_store_clear_pending_alarm_delete(pending_alarms.pending_delete_ids[i]);
                    }
                }
            }
        }

        if (s_force_ota_check ||
            (local.ota_enabled &&
             now_us - last_ota_us >= (int64_t)CONFIG_CS_OTA_CHECK_INTERVAL_SEC * 1000000LL)) {
            s_force_ota_check = false;
            last_ota_us = now_us;
            if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                snprintf(s_shared.ota_status, sizeof(s_shared.ota_status), "checking");
                s_shared.ota_detail[0] = '\0';
                xSemaphoreGive(s_state_mutex);
            }
            bool update_avail = false;
            char detail[64] = "";
            esp_err_t ota_err = api_client_check_and_run_ota(
                CONFIG_CS_FIRMWARE_VERSION, &update_avail, detail, sizeof(detail));
            if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                snprintf(s_shared.ota_status, sizeof(s_shared.ota_status), "%s",
                         (ota_err == ESP_OK) ? (update_avail ? "updating" : "up_to_date") : "error");
                snprintf(s_shared.ota_detail, sizeof(s_shared.ota_detail), "%s", detail);
                xSemaphoreGive(s_state_mutex);
            }
        }

        if (s_force_cfg_sync ||
            now_us - last_cfg_us >= (int64_t)CONFIG_CS_API_CONFIG_SYNC_SEC * 1000000LL) {
            bool force_apps_sync = s_force_cfg_sync;
            s_force_cfg_sync = false;
            static bool s_apps_meta_synced = false;
            if (!s_apps_meta_synced || force_apps_sync) {
                if (api_client_sync_apps_metadata() == ESP_OK) {
                    s_apps_meta_synced = true;
                }
            }
            cs_device_config_t new_cfg;
            if (api_client_fetch_config(&new_cfg) == ESP_OK) {
                bool bday_act = false;
                bool hol_act = false;
                if (api_client_fetch_active_events(&bday_act, &hol_act) == ESP_OK) {
                    new_cfg.active_events_known = true;
                    new_cfg.active_event_birthdays = bday_act;
                    new_cfg.active_event_holidays = hol_act;
                }
                last_cfg_us = now_us;
                sntp_time_set_timezone(new_cfg.timezone);
                settings_store_get(&local);
                /* Local NVS owns brightness mode (cloud /config cannot clear override back to Auto/null). */
                new_cfg.brightness_override = local.brightness_override;

                if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    if (!new_cfg.enabled_apps_present) {
                        new_cfg.enabled_apps_present = s_shared.cfg.enabled_apps_present;
                        new_cfg.enabled_app_count = s_shared.cfg.enabled_app_count;
                        memcpy(new_cfg.enabled_apps, s_shared.cfg.enabled_apps,
                               sizeof(new_cfg.enabled_apps));
                    }
                    if (!new_cfg.active_events_known && s_shared.cfg.active_events_known) {
                        new_cfg.active_events_known = s_shared.cfg.active_events_known;
                        new_cfg.active_event_birthdays = s_shared.cfg.active_event_birthdays;
                        new_cfg.active_event_holidays = s_shared.cfg.active_event_holidays;
                    }
                    bool apps_changed = (s_shared.cfg.enabled_app_count != new_cfg.enabled_app_count);
                    if (!apps_changed) {
                        for (size_t i = 0; i < new_cfg.enabled_app_count; i++) {
                            if (strcmp(s_shared.cfg.enabled_apps[i], new_cfg.enabled_apps[i]) != 0) {
                                apps_changed = true;
                                break;
                            }
                        }
                    }
                    bool cfg_changed =
                        apps_changed ||
                        (strcmp(s_shared.cfg.favorite_teams, new_cfg.favorite_teams) != 0) ||
                        (strcmp(s_shared.cfg.timezone, new_cfg.timezone) != 0) ||
                        (s_shared.cfg.quiet_enabled != new_cfg.quiet_enabled) ||
                        (strcmp(s_shared.cfg.quiet_start, new_cfg.quiet_start) != 0) ||
                        (strcmp(s_shared.cfg.quiet_end, new_cfg.quiet_end) != 0);
                    if (cfg_changed) {
                        cache_invalidate_all_locked();
                    }
                    s_shared.cfg = new_cfg;
#if CONFIG_CS_ENABLE_LIGHT_SENSOR
                    s_shared.auto_brightness = (local.brightness_override < 0);
#endif
                    if (!s_shared.rotary_override_active) {
                        rebuild_rotary_slots_locked(local.pinned_app);
                        if (local.pinned_app[0] != '\0' && s_shared.selected_app_idx < 0) {
                            snprintf(s_shared.manual_pinned_slot,
                                     sizeof(s_shared.manual_pinned_slot),
                                     "%s", local.pinned_app);
                        } else {
                            s_shared.manual_pinned_slot[0] = '\0';
                        }
                    } else {
                        rebuild_rotary_slots_locked(NULL);
                    }
                    xSemaphoreGive(s_state_mutex);
                }

                /* Keep local NVS aligned if cloud-owned fields changed on server */
                if (!local.cloud_sync_pending) {
                    bool nvs_changed = false;
                    if (strcmp(local.favorite_teams, new_cfg.favorite_teams) != 0) {
                        snprintf(local.favorite_teams, sizeof(local.favorite_teams),
                                 "%s", new_cfg.favorite_teams);
                        nvs_changed = true;
                    }
                    if (strcmp(local.device_name, new_cfg.device_name) != 0) {
                        snprintf(local.device_name, sizeof(local.device_name), "%s", new_cfg.device_name);
                        nvs_changed = true;
                    }
                    if (strcmp(local.timezone, new_cfg.timezone) != 0) {
                        snprintf(local.timezone, sizeof(local.timezone), "%s", new_cfg.timezone);
                        nvs_changed = true;
                    }
                    if (strcmp(local.quiet_start, new_cfg.quiet_start) != 0) {
                        snprintf(local.quiet_start, sizeof(local.quiet_start), "%s", new_cfg.quiet_start);
                        nvs_changed = true;
                    }
                    if (strcmp(local.quiet_end, new_cfg.quiet_end) != 0) {
                        snprintf(local.quiet_end, sizeof(local.quiet_end), "%s", new_cfg.quiet_end);
                        nvs_changed = true;
                    }
                    if (local.quiet_enabled != new_cfg.quiet_enabled) {
                        local.quiet_enabled = new_cfg.quiet_enabled;
                        nvs_changed = true;
                    }
                    if (nvs_changed) {
                        settings_store_save(&local);
                    }
                }
            }
        }

        /* Brief settle window when knob was just turned so rapid multi-click turns fetch only the target app */
        int64_t rotary_age_us = 0;
        if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (s_shared.last_rotary_us > 0) {
                rotary_age_us = esp_timer_get_time() - s_shared.last_rotary_us;
            }
            xSemaphoreGive(s_state_mutex);
        }
        if (rotary_age_us > 0 && rotary_age_us < 120000LL) {
            uint32_t settle_ms = (uint32_t)((120000LL - rotary_age_us + 999LL) / 1000LL);
            if (settle_ms > 0U) {
                vTaskDelay(pdMS_TO_TICKS(settle_ms));
            }
        }

        int req_app_idx = -1;
        char req_slot_key[64] = {0};
        char app_override[32] = {0};
        char screen_override[24] = {0};
        char cached_etag[48] = {0};
        char etag_lookup_key[64] = {0};
        if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            req_app_idx = s_shared.selected_app_idx;
            if (s_shared.manual_pinned_slot[0] != '\0') {
                snprintf(req_slot_key, sizeof(req_slot_key), "%s", s_shared.manual_pinned_slot);
            } else if (req_app_idx >= 0 &&
                       req_app_idx < (int)s_shared.rotary_slot_count) {
                snprintf(req_slot_key, sizeof(req_slot_key), "%s",
                         s_shared.rotary_slots[req_app_idx].key);
            }
            split_slot_key(req_slot_key, app_override, sizeof(app_override),
                           screen_override, sizeof(screen_override));
            if (s_shared.has_frame) {
                const char *lk = req_slot_key[0] ? req_slot_key : s_shared.auto_app_id;
                if (lk && lk[0]) {
                    snprintf(etag_lookup_key, sizeof(etag_lookup_key), "%s", lk);
                    cache_get_etag_locked(lk, cached_etag, sizeof(cached_etag));
                }
            }
            xSemaphoreGive(s_state_mutex);
        }

        settings_store_get(&local);
        const char *teams_arg = (local.cloud_sync_pending && local.favorite_teams[0])
                                    ? local.favorite_teams
                                    : NULL;

        uint32_t base_poll = (local.poll_interval_sec >= 1U)
                                 ? local.poll_interval_sec
                                 : CONFIG_CS_API_POLL_INTERVAL_SEC;
        static bool s_logged_health = false;
        cs_frame_response_t *resp = psram_prefer_calloc(1, sizeof(cs_frame_response_t));
        uint32_t wait_sec = base_poll;
        bool stale_selection = false;
        bool fetched_ok = false;
        bool need_render_wake = false;

        if (!resp) {
            ESP_LOGE(TAG, "Out of memory allocating cs_frame_response_t");
            wait_sec = 5;
        } else if (api_client_fetch_frame_ex(app_override[0] ? app_override : NULL,
                                             screen_override[0] ? screen_override : NULL,
                                             teams_arg,
                                             true,
                                             cached_etag[0] ? cached_etag : NULL,
                                             resp) == ESP_OK) {
            fetched_ok = true;
            fail_count = 0;
            hub75_set_status_indicator(HUB75_STATUS_NONE);
            wait_sec = (resp->refresh_seconds >= 1U)
                           ? resp->refresh_seconds
                           : base_poll;

            char store_key[64] = {0};
            if (req_slot_key[0] != '\0') {
                snprintf(store_key, sizeof(store_key), "%s", req_slot_key);
            } else if (resp->app_id[0] != '\0') {
                snprintf(store_key, sizeof(store_key), "%s", resp->app_id);
            } else if (etag_lookup_key[0] != '\0') {
                snprintf(store_key, sizeof(store_key), "%s", etag_lookup_key);
            }

            if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                s_shared.api_fail_count = 0;
                if (resp->not_modified) {
                    if (store_key[0]) {
                        cache_touch_slot_locked(store_key, resp->refresh_seconds);
                    }
                    const char *cur_slot_key = "";
                    if (s_shared.manual_pinned_slot[0] != '\0') {
                        cur_slot_key = s_shared.manual_pinned_slot;
                    } else if (s_shared.selected_app_idx >= 0 &&
                               s_shared.selected_app_idx < (int)s_shared.rotary_slot_count) {
                        cur_slot_key = s_shared.rotary_slots[s_shared.selected_app_idx].key;
                    }
                    if (s_shared.selected_app_idx != req_app_idx ||
                        strcmp(cur_slot_key, req_slot_key) != 0) {
                        stale_selection = true;
                    } else if (s_shared.switching_pending ||
                               (store_key[0] && strcmp(s_shared.active_app_id, store_key) != 0)) {
                        cache_apply_app_locked(store_key[0] ? store_key : NULL);
                        need_render_wake = true;
                    }
                } else {
                    cache_store_frame_locked(store_key,
                                             resp->bg_rgb565be,
                                             resp->etag,
                                             resp->overlays,
                                             resp->overlay_count,
                                             resp->quiet_mode,
                                             resp->refresh_seconds);
                    if (req_slot_key[0] == '\0' && resp->app_id[0]) {
                        snprintf(s_shared.auto_app_id, sizeof(s_shared.auto_app_id),
                                 "%s", resp->app_id);
                    }

                    const char *cur_slot_key = "";
                    if (s_shared.manual_pinned_slot[0] != '\0') {
                        cur_slot_key = s_shared.manual_pinned_slot;
                    } else if (s_shared.selected_app_idx >= 0 &&
                               s_shared.selected_app_idx < (int)s_shared.rotary_slot_count) {
                        cur_slot_key = s_shared.rotary_slots[s_shared.selected_app_idx].key;
                    }
                    if (s_shared.selected_app_idx != req_app_idx ||
                        strcmp(cur_slot_key, req_slot_key) != 0) {
                        stale_selection = true;
                        api_client_free_alert(&resp->alert);
                    } else {
                        bool app_changed = (strcmp(s_shared.active_app_id, store_key) != 0);
                        if (s_shared.bg_rgb888) {
                            memcpy(s_shared.bg_rgb888, resp->bg_rgb888, CS_FRAME_RGB888_BYTES);
                        }
                        snprintf(s_shared.active_app_id, sizeof(s_shared.active_app_id),
                                 "%s", store_key);
                        s_shared.overlay_count = resp->overlay_count;
                        if (resp->overlay_count > 0) {
                            memcpy(s_shared.overlays, resp->overlays,
                                   resp->overlay_count * sizeof(cs_overlay_t));
                        }
                        s_shared.last_quiet_mode = resp->quiet_mode;
                        s_shared.has_frame = true;
                        s_shared.switching_pending = false;
                        s_shared.switching_label[0] = '\0';
                        need_render_wake = true;

                        if (resp->alert.valid) {
                            api_client_free_alert(&s_shared.pending_alert);
                            s_shared.pending_alert = resp->alert;
                            memset(&resp->alert, 0, sizeof(resp->alert));
                        }
                        if (app_changed || req_slot_key[0]) {
                            ESP_LOGI(TAG, "Frame active: slot=%s (resp=%s)",
                                     store_key, resp->app_id);
                        }
                    }
                }
                xSemaphoreGive(s_state_mutex);
            } else {
                api_client_free_alert(&resp->alert);
            }

            if (!s_logged_health) {
                s_logged_health = true;
                ESP_LOGI(TAG, "API health: internal_free=%lu internal_largest=%lu api_stack_hwm=%lu",
                         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                         (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                         (unsigned long)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
            }

            free(resp);
            resp = NULL;

            if (!stale_selection && need_render_wake && s_render_task) {
                xTaskNotifyGive(s_render_task);
            }
        } else {
            api_client_free_alert(&resp->alert);
            free(resp);
            resp = NULL;
            fail_count++;
            if (fail_count == 1U || (fail_count % 5U) == 0U) {
                ESP_LOGW(TAG, "Frame fetch failed (#%lu): internal_free=%lu internal_largest=%lu api_stack_hwm=%lu",
                         (unsigned long)fail_count,
                         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                         (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                         (unsigned long)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
            }
            if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                s_shared.api_fail_count = fail_count;
                s_shared.switching_pending = false;
                xSemaphoreGive(s_state_mutex);
            }
            uint32_t shift = (fail_count - 1U > 4U) ? 4U : (fail_count - 1U);
            wait_sec = CONFIG_CS_API_POLL_INTERVAL_SEC << shift;
            if (wait_sec > CONFIG_CS_API_MAX_BACKOFF_SEC) {
                wait_sec = CONFIG_CS_API_MAX_BACKOFF_SEC;
            }
            hub75_set_status_indicator((fail_count >= CONFIG_CS_API_ERROR_BLINK_THRESHOLD)
                                           ? HUB75_STATUS_ERROR
                                           : HUB75_STATUS_RETRYING);
            if (s_render_task) {
                xTaskNotifyGive(s_render_task);
            }
        }

        free(resp);
        resp = NULL;
        if (stale_selection) {
            continue;
        }

        /* Background-warm / refresh one uncached or TTL-expired slot per idle poll so every screen stays fresh */
        if (fetched_ok && req_slot_key[0] == '\0') {
            char warm_slot[64] = {0};
            if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                if (s_shared.selected_app_idx < 0 && s_shared.manual_pinned_slot[0] == '\0') {
                    cache_pick_bg_refresh_app_locked(esp_timer_get_time(), warm_slot, sizeof(warm_slot));
                }
                xSemaphoreGive(s_state_mutex);
            }
            if (warm_slot[0]) {
                char warm_app[32] = {0};
                char warm_screen[24] = {0};
                char warm_etag[48] = {0};
                split_slot_key(warm_slot, warm_app, sizeof(warm_app),
                               warm_screen, sizeof(warm_screen));
                if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    cache_get_etag_locked(warm_slot, warm_etag, sizeof(warm_etag));
                    xSemaphoreGive(s_state_mutex);
                }
                cs_frame_response_t *wresp = psram_prefer_calloc(1, sizeof(cs_frame_response_t));
                if (wresp) {
                    if (api_client_fetch_frame_ex(warm_app,
                                                  warm_screen[0] ? warm_screen : NULL,
                                                  teams_arg, false,
                                                  warm_etag[0] ? warm_etag : NULL,
                                                  wresp) == ESP_OK) {
                        if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                            if (wresp->not_modified) {
                                cache_touch_slot_locked(warm_slot, wresp->refresh_seconds);
                            } else {
                                cache_store_frame_locked(warm_slot,
                                                         wresp->bg_rgb565be,
                                                         wresp->etag,
                                                         wresp->overlays,
                                                         wresp->overlay_count,
                                                         wresp->quiet_mode,
                                                         wresp->refresh_seconds);
                            }
                            xSemaphoreGive(s_state_mutex);
                        }
                        ESP_LOGD(TAG, "Refreshed cached slot frame: %s (%s)",
                                 warm_slot, wresp->not_modified ? "304" : "200");
                    }
                    api_client_free_alert(&wresp->alert);
                    free(wresp);
                }
                bool knob_moved = false;
                if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                    knob_moved = (s_shared.selected_app_idx >= 0 ||
                                  s_shared.manual_pinned_slot[0] != '\0');
                    xSemaphoreGive(s_state_mutex);
                }
                if (knob_moved) {
                    continue;
                }
            }
        }

        maybe_play_hourly_chime(&last_chime_hour);

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_sec * 1000U));
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(settings_store_init());

    s_state_mutex = xSemaphoreCreateMutex();
    configASSERT(s_state_mutex != NULL);

    s_shared.bg_rgb888 = psram_prefer_calloc(1, CS_FRAME_RGB888_BYTES);
    configASSERT(s_shared.bg_rgb888 != NULL);
    s_frame_cache = psram_prefer_calloc(CS_MAX_ROTARY_SLOTS, sizeof(app_frame_cache_entry_t));

    init_default_config(&s_shared.cfg);
    s_shared.selected_app_idx = -1;
    {
        cs_local_settings_t boot_local;
        settings_store_get(&boot_local);
        s_shared.rotary_idle_sec = boot_local.rotary_idle_sec;
        rebuild_rotary_slots_locked(boot_local.pinned_app);
        if (boot_local.pinned_app[0] != '\0' && s_shared.selected_app_idx < 0) {
            snprintf(s_shared.manual_pinned_slot, sizeof(s_shared.manual_pinned_slot),
                     "%s", boot_local.pinned_app);
        }
    }
#if CONFIG_CS_ENABLE_LIGHT_SENSOR
    s_shared.auto_brightness = (s_shared.cfg.brightness_override < 0);
#else
    s_shared.auto_brightness = false;
#endif

    ESP_ERROR_CHECK(hub75_init());
    if (s_shared.cfg.brightness_override >= 0) {
        hub75_set_brightness((uint8_t)s_shared.cfg.brightness_override);
    }

#if CONFIG_CS_ENABLE_DFPLAYER
    if (dfplayer_init() != ESP_OK) {
        ESP_LOGW(TAG, "DFPlayer initialization failed (continuing without audio)");
    } else {
        cs_local_settings_t local;
        settings_store_get(&local);
        s_shared.menu_vol = local.df_volume;
        dfplayer_set_volume(local.df_volume);
        vTaskDelay(pdMS_TO_TICKS(30));
        dfplayer_set_eq(local.df_eq);
    }
#endif

    QueueHandle_t input_queue = NULL;
    if (peripherals_init(&input_queue) != ESP_OK) {
        ESP_LOGW(TAG, "Peripherals initialization failed (continuing without inputs)");
    }

    ESP_ERROR_CHECK(wifi_manager_init());

    const web_portal_hooks_t portal_hooks = {
        .get_display_status = portal_get_status,
        .get_ota_status = portal_get_ota_status,
        .get_active_events = portal_get_active_events,
        .is_alarm_ringing = portal_is_alarm_ringing,
        .get_api_fail_count = portal_get_api_fail_count,
        .on_settings_updated = portal_on_settings_updated,
        .on_apply_brightness = portal_on_apply_brightness,
        .on_set_pinned_app = portal_on_set_pinned_app,
        .on_step_app = portal_on_step_app,
        .on_apps_metadata_synced = portal_on_apps_metadata_synced,
        .on_request_refresh = portal_on_request_refresh,
        .on_request_ota = portal_on_request_ota,
        .on_audio_action = portal_on_audio_action,
    };
    if (web_portal_start(&portal_hooks) != ESP_OK) {
        ESP_LOGW(TAG, "Local web portal failed to start");
    }

    xTaskCreatePinnedToCore(render_task, "render_task", 8192, NULL, 5, &s_render_task, 1);
    xTaskCreate(input_task, "input_task", 4096, (void *)input_queue, 6, NULL);
    xTaskCreate(api_task, "api_task", 16384, NULL, 4, &s_api_task);

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    if (running && esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t ota_valid_err = esp_ota_mark_app_valid_cancel_rollback();
        if (ota_valid_err == ESP_OK) {
            ESP_LOGI(TAG, "OTA image verified after successful boot; rollback canceled");
        } else {
            ESP_LOGW(TAG, "Failed to confirm OTA image: %s", esp_err_to_name(ota_valid_err));
        }
    }
}
