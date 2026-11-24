#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "settings_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*get_display_status)(char *active_app, size_t active_cap,
                               char *pinned_app, size_t pinned_cap,
                               char *apps_json_arr, size_t apps_cap,
                               uint8_t *brightness_out,
                               bool *quiet_mode_out);
    void (*get_ota_status)(char *state, size_t state_cap,
                           char *detail, size_t detail_cap);
    void (*get_temp_override)(char *temp_app, size_t cap,
                              uint32_t *remaining_sec_out);
    bool (*get_active_events)(bool *birthdays_active, bool *holidays_active);
    bool (*is_alarm_ringing)(void);
    uint32_t (*get_api_fail_count)(void);
    void (*on_settings_updated)(const cs_local_settings_t *settings, const char *pinned_app);
    void (*on_apply_brightness)(int brightness_override);
    void (*on_set_pinned_app)(const char *app_id);
    void (*on_show_temp_app)(const char *app_id, uint32_t duration_sec);
    void (*on_step_app)(int delta);
    void (*on_apps_metadata_synced)(void);
    void (*on_request_refresh)(void);
    void (*on_request_ota)(void);
    void (*on_audio_action)(const char *action);
    bool (*is_admin_unlocked)(void);
    bool (*is_admin_pin_active)(void);
    void (*on_request_admin_pin)(void);
    bool (*on_verify_admin_pin)(const char *pin);
} web_portal_hooks_t;

/**
 * Start the local HTTP configuration and captive portal web server on port 80.
 */
esp_err_t web_portal_start(const web_portal_hooks_t *hooks);

#ifdef __cplusplus
}
#endif
