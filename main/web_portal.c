#include "web_portal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#include "api_client.h"
#include "hub75_driver.h"
#include "json_min.h"
#include "peripherals.h"
#include "sntp_time.h"
#include "wifi_manager.h"

static const char *TAG = "web_portal";

extern const uint8_t web_ui_html_start[] asm("_binary_web_ui_html_start");
extern const uint8_t web_ui_html_end[] asm("_binary_web_ui_html_end");

static httpd_handle_t s_server = NULL;
static web_portal_hooks_t s_hooks;

typedef enum {
    DEFER_NONE = 0,
    DEFER_APPLY_STA,
    DEFER_ENTER_SETUP,
    DEFER_REBOOT,
    DEFER_FACTORY_RESET,
} deferred_op_t;

static esp_timer_handle_t s_defer_timer = NULL;
static deferred_op_t s_defer_op = DEFER_NONE;
static char s_defer_ssid[33];
static char s_defer_pass[65];

static void deferred_op_cb(void *arg)
{
    (void)arg;
    deferred_op_t op = s_defer_op;
    s_defer_op = DEFER_NONE;

    if (op == DEFER_APPLY_STA) {
        wifi_manager_apply_sta_credentials(s_defer_ssid, s_defer_pass, true);
    } else if (op == DEFER_ENTER_SETUP) {
        wifi_manager_enter_setup_mode();
    } else if (op == DEFER_REBOOT) {
        esp_restart();
    } else if (op == DEFER_FACTORY_RESET) {
        settings_store_factory_reset();
        esp_wifi_restore();
        esp_restart();
    }
}

static void schedule_deferred(deferred_op_t op, uint32_t delay_ms)
{
    if (!s_defer_timer) {
        const esp_timer_create_args_t args = {
            .callback = deferred_op_cb,
            .name = "portal_defer",
        };
        if (esp_timer_create(&args, &s_defer_timer) != ESP_OK) {
            return;
        }
    }
    s_defer_op = op;
    esp_timer_stop(s_defer_timer);
    esp_timer_start_once(s_defer_timer, (uint64_t)delay_ms * 1000ULL);
}

static void sanitize_teams_csv(char *s)
{
    if (!s) {
        return;
    }
    size_t w = 0;
    for (size_t r = 0; s[r] != '\0'; r++) {
        unsigned char c = (unsigned char)s[r];
        if (isalnum(c) || c == ',') {
            s[w++] = (char)toupper(c);
        }
    }
    while (w > 0 && s[w - 1] == ',') {
        w--;
    }
    s[w] = '\0';
}

static void trim_url(char *s)
{
    if (!s) {
        return;
    }
    size_t start = 0;
    while (s[start] && isspace((unsigned char)s[start])) {
        start++;
    }
    if (start > 0) {
        memmove(s, s + start, strlen(s + start) + 1U);
    }
    size_t len = strlen(s);
    while (len > 0 && (isspace((unsigned char)s[len - 1]) || s[len - 1] == '/')) {
        s[--len] = '\0';
    }
}

static size_t append_json_str(char *dst, size_t cap, size_t pos, const char *s)
{
    if (pos < cap) {
        dst[pos++] = '"';
    }
    if (s) {
        for (size_t i = 0; s[i] != '\0' && pos + 6 < cap; i++) {
            unsigned char c = (unsigned char)s[i];
            if (c == '"' || c == '\\') {
                dst[pos++] = '\\';
                dst[pos++] = (char)c;
            } else if (c == '\n') {
                dst[pos++] = '\\';
                dst[pos++] = 'n';
            } else if (c == '\r') {
                dst[pos++] = '\\';
                dst[pos++] = 'r';
            } else if (c >= 0x20) {
                dst[pos++] = (char)c;
            }
        }
    }
    if (pos < cap) {
        dst[pos++] = '"';
    }
    if (pos < cap) {
        dst[pos] = '\0';
    } else if (cap > 0) {
        dst[cap - 1] = '\0';
    }
    return pos;
}

static esp_err_t read_req_body(httpd_req_t *req, size_t max_len, char **out_buf, size_t *out_len)
{
    *out_buf = NULL;
    *out_len = 0;
    if (req->content_len <= 0 || (size_t)req->content_len > max_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *buf = malloc((size_t)req->content_len + 1U);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    size_t got = 0;
    while (got < (size_t)req->content_len) {
        int r = httpd_req_recv(req, buf + got, (size_t)req->content_len - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            free(buf);
            return ESP_FAIL;
        }
        got += (size_t)r;
    }
    buf[got] = '\0';
    *out_buf = buf;
    *out_len = got;
    return ESP_OK;
}

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    size_t html_len = (size_t)(web_ui_html_end - web_ui_html_start);
    if (html_len > 0 && web_ui_html_start[html_len - 1] == '\0') {
        html_len--;
    }
    return httpd_resp_send(req, (const char *)web_ui_html_start, (ssize_t)html_len);
}

static esp_err_t handle_captive_404(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    if (wifi_manager_is_ap_mode()) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, "Redirecting to Clean Slate Setup...", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_OK;
}

static esp_err_t handle_get_status(httpd_req_t *req)
{
    cs_local_settings_t cfg;
    settings_store_get(&cfg);

    char ip[16] = {0};
    char dns_ip[16] = {0};
    char sta_ssid[33] = {0};
    wifi_manager_get_ip_str(ip, sizeof(ip));
    wifi_manager_get_dns_str(dns_ip, sizeof(dns_ip));
    wifi_manager_get_sta_ssid(sta_ssid, sizeof(sta_ssid));

    char active_app[32] = "clock";
    char pinned_app[32] = "";
    char apps_arr[256] = "[]";
    char ota_state[24] = "idle";
    char ota_detail[64] = "";
    uint8_t brightness = 128;
    bool quiet_mode = false;
    if (s_hooks.get_display_status) {
        s_hooks.get_display_status(active_app, sizeof(active_app),
                                   pinned_app, sizeof(pinned_app),
                                   apps_arr, sizeof(apps_arr),
                                   &brightness, &quiet_mode);
    }
    if (s_hooks.get_ota_status) {
        s_hooks.get_ota_status(ota_state, sizeof(ota_state),
                               ota_detail, sizeof(ota_detail));
    }

    cs_api_last_status_t api_st;
    api_client_get_last_status(&api_st);
    uint32_t api_fail_count = s_hooks.get_api_fail_count ? s_hooks.get_api_fail_count() : 0U;

    char *out = malloc(3072);
    if (!out) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_ERR_NO_MEM;
    }

    size_t cap = 3072;
    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, cap - pos,
                            "{\"ap_mode\":%s,\"ap_ssid\":",
                            wifi_manager_is_ap_mode() ? "true" : "false");
    pos = append_json_str(out, cap, pos, wifi_manager_get_ap_ssid());
    pos += (size_t)snprintf(out + pos, cap - pos,
                            ",\"wifi_connected\":%s,\"wifi_ssid\":",
                            wifi_manager_is_connected() ? "true" : "false");
    pos = append_json_str(out, cap, pos, sta_ssid);
    pos += (size_t)snprintf(out + pos, cap - pos, ",\"ip\":");
    pos = append_json_str(out, cap, pos, ip);
    pos += (size_t)snprintf(out + pos, cap - pos, ",\"dns_ip\":");
    pos = append_json_str(out, cap, pos, dns_ip);
    pos += (size_t)snprintf(out + pos, cap - pos, ",\"hostname\":");
    pos = append_json_str(out, cap, pos, wifi_manager_get_device_id());
    pos += (size_t)snprintf(out + pos, cap - pos, ",\"device_id\":");
    pos = append_json_str(out, cap, pos, wifi_manager_get_device_id());
    pos += (size_t)snprintf(out + pos, cap - pos,
                            ",\"rssi\":%d,\"sntp_synced\":%s,\"free_heap\":%lu,\"largest_free_block\":%lu,\"internal_free_heap\":%lu,\"internal_largest_block\":%lu,\"firmware_version\":",
                            wifi_manager_get_rssi(),
                            sntp_time_is_synced() ? "true" : "false",
                            (unsigned long)esp_get_free_heap_size(),
                            (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                            (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                            (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    pos = append_json_str(out, cap, pos, CONFIG_CS_FIRMWARE_VERSION);
    pos += (size_t)snprintf(out + pos, cap - pos, ",\"active_app\":");
    pos = append_json_str(out, cap, pos, active_app);
    pos += (size_t)snprintf(out + pos, cap - pos, ",\"pinned_app\":");
    pos = append_json_str(out, cap, pos, pinned_app);
    pos += (size_t)snprintf(out + pos, cap - pos,
                            ",\"available_apps\":%s,\"quiet_mode\":%s,\"brightness\":%u,\"ambient_raw\":%d,\"ota_status\":",
                            apps_arr, quiet_mode ? "true" : "false",
                            (unsigned)brightness, peripherals_get_ambient_raw());
    pos = append_json_str(out, cap, pos, ota_state);
    pos += (size_t)snprintf(out + pos, cap - pos, ",\"ota_detail\":");
    pos = append_json_str(out, cap, pos, ota_detail);
    pos += (size_t)snprintf(out + pos, cap - pos,
                            ",\"api_status\":{\"attempted\":%s,\"ok\":%s,\"fail_count\":%lu,"
                            "\"http_status\":%d,\"err\":",
                            api_st.attempted ? "true" : "false",
                            api_st.ok ? "true" : "false",
                            (unsigned long)api_fail_count,
                            api_st.http_status);
    pos = append_json_str(out, cap, pos, api_st.attempted ? esp_err_to_name(api_st.err) : "none");
    pos += (size_t)snprintf(out + pos, cap - pos,
                            ",\"tls_err\":%d,\"cert_flags\":%lu,\"sock_errno\":%d,"
                            "\"latency_ms\":%lu,\"last_ok_age_sec\":%d,\"endpoint\":",
                            api_st.tls_err,
                            (unsigned long)api_st.cert_flags,
                            api_st.sock_errno,
                            (unsigned long)api_st.latency_ms,
                            api_st.last_ok_age_sec);
    pos = append_json_str(out, cap, pos, api_st.endpoint);
    pos += (size_t)snprintf(out + pos, cap - pos, ",\"stage\":");
    pos = append_json_str(out, cap, pos, api_st.stage);
    pos += (size_t)snprintf(out + pos, cap - pos, ",\"detail\":");
    pos = append_json_str(out, cap, pos, api_st.detail);
    pos += (size_t)snprintf(out + pos, cap - pos, "}");

    pos += (size_t)snprintf(out + pos, cap - pos, ",\"settings\":{\"wifi_ssid\":");
    pos = append_json_str(out, cap, pos, cfg.wifi_ssid);
    pos += (size_t)snprintf(out + pos, cap - pos, ",\"device_name\":");
    pos = append_json_str(out, cap, pos, cfg.device_name);
    pos += (size_t)snprintf(out + pos, cap - pos, ",\"favorite_teams\":");
    pos = append_json_str(out, cap, pos, cfg.favorite_teams);
    pos += (size_t)snprintf(out + pos, cap - pos, ",\"api_base_url\":");
    pos = append_json_str(out, cap, pos, cfg.api_base_url);
    pos += (size_t)snprintf(out + pos, cap - pos, ",\"timezone\":");
    pos = append_json_str(out, cap, pos, cfg.timezone);
    pos += (size_t)snprintf(out + pos, cap - pos, ",\"ntp_server\":");
    pos = append_json_str(out, cap, pos, cfg.ntp_server);
    pos += (size_t)snprintf(out + pos, cap - pos, ",\"quiet_enabled\":%s,\"quiet_start\":",
                            cfg.quiet_enabled ? "true" : "false");
    pos = append_json_str(out, cap, pos, cfg.quiet_start);
    pos += (size_t)snprintf(out + pos, cap - pos, ",\"quiet_end\":");
    pos = append_json_str(out, cap, pos, cfg.quiet_end);
    pos += (size_t)snprintf(out + pos, cap - pos,
                            ",\"brightness_override\":%d,\"brightness_min\":%u,\"brightness_max\":%u,"
                            "\"df_volume\":%u,\"df_eq\":%u,"
                            "\"boot_tone\":%s,\"hourly_chime\":%s,"
                            "\"rotary_idle_sec\":%u,\"poll_interval_sec\":%u,\"ota_enabled\":%s,"
                            "\"fight_folder\":%u,\"fight_track\":%u,"
                            "\"victory_folder\":%u,\"victory_track\":%u,"
                            "\"third_folder\":%u,\"third_track\":%u}}",
                            cfg.brightness_override,
                            (unsigned)cfg.brightness_min,
                            (unsigned)cfg.brightness_max,
                            (unsigned)cfg.df_volume,
                            (unsigned)cfg.df_eq,
                            cfg.boot_tone ? "true" : "false",
                            cfg.hourly_chime ? "true" : "false",
                            (unsigned)cfg.rotary_idle_sec,
                            (unsigned)cfg.poll_interval_sec,
                            cfg.ota_enabled ? "true" : "false",
                            (unsigned)cfg.fight_folder,
                            (unsigned)cfg.fight_track,
                            (unsigned)cfg.victory_folder,
                            (unsigned)cfg.victory_track,
                            (unsigned)cfg.third_folder,
                            (unsigned)cfg.third_track);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t res = httpd_resp_send(req, out, (ssize_t)strlen(out));
    free(out);
    return res;
}

static esp_err_t handle_get_scan(httpd_req_t *req)
{
    wifi_scan_entry_t *entries = calloc(16, sizeof(wifi_scan_entry_t));
    if (!entries) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_ERR_NO_MEM;
    }

    size_t count = 0;
    esp_err_t err = wifi_manager_scan_networks(entries, 16, &count);
    if (err != ESP_OK) {
        free(entries);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan failed");
        return err;
    }

    size_t cap = 1536;
    char *out = malloc(cap);
    if (!out) {
        free(entries);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_ERR_NO_MEM;
    }

    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, cap - pos, "[");
    for (size_t i = 0; i < count && pos + 80 < cap; i++) {
        if (i > 0) {
            out[pos++] = ',';
        }
        pos += (size_t)snprintf(out + pos, cap - pos, "{\"ssid\":");
        pos = append_json_str(out, cap, pos, entries[i].ssid);
        pos += (size_t)snprintf(out + pos, cap - pos, ",\"rssi\":%d,\"open\":%s}",
                                (int)entries[i].rssi, entries[i].open ? "true" : "false");
    }
    pos += (size_t)snprintf(out + pos, (pos < cap) ? (cap - pos) : 0, "]");

    free(entries);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t res = httpd_resp_send(req, out, (ssize_t)strlen(out));
    free(out);
    return res;
}

static esp_err_t handle_post_setup(httpd_req_t *req)
{
    char *body = NULL;
    size_t body_len = 0;
    esp_err_t err = read_req_body(req, 2048, &body, &body_len);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid setup payload");
        return err;
    }

    json_span_t root = json_root(body, body_len);
    json_span_t fv;

    cs_local_settings_t cfg;
    settings_store_get(&cfg);

    if (!json_obj_get(root, "wifi_ssid", &fv) ||
        !json_as_str(fv, cfg.wifi_ssid, sizeof(cfg.wifi_ssid)) ||
        cfg.wifi_ssid[0] == '\0') {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "wifi_ssid is required");
        return ESP_ERR_INVALID_ARG;
    }

    if (json_obj_get(root, "wifi_pass", &fv)) {
        json_as_str(fv, cfg.wifi_pass, sizeof(cfg.wifi_pass));
    } else {
        cfg.wifi_pass[0] = '\0';
    }

    if (json_obj_get(root, "favorite_team", &fv)) {
        json_as_str(fv, cfg.favorite_teams, sizeof(cfg.favorite_teams));
    } else if (json_obj_get(root, "favorite_teams", &fv)) {
        json_as_str(fv, cfg.favorite_teams, sizeof(cfg.favorite_teams));
    }
    sanitize_teams_csv(cfg.favorite_teams);

    if (json_obj_get(root, "api_base_url", &fv)) {
        json_as_str(fv, cfg.api_base_url, sizeof(cfg.api_base_url));
    }
    trim_url(cfg.api_base_url);
    cfg.cloud_sync_pending = true;
    free(body);

    err = settings_store_save(&cfg);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed saving NVS");
        return err;
    }

    if (s_hooks.on_settings_updated) {
        s_hooks.on_settings_updated(&cfg, NULL);
    }

    snprintf(s_defer_ssid, sizeof(s_defer_ssid), "%s", cfg.wifi_ssid);
    snprintf(s_defer_pass, sizeof(s_defer_pass), "%s", cfg.wifi_pass);
    schedule_deferred(DEFER_APPLY_STA, 600);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"mode\":\"connecting\"}");
}

static esp_err_t handle_post_settings(httpd_req_t *req)
{
    char *body = NULL;
    size_t body_len = 0;
    esp_err_t err = read_req_body(req, 4096, &body, &body_len);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid settings payload");
        return err;
    }

    json_span_t root = json_root(body, body_len);
    json_span_t fv;

    cs_local_settings_t cfg;
    settings_store_get(&cfg);

    if (json_obj_get(root, "device_name", &fv)) {
        json_as_str(fv, cfg.device_name, sizeof(cfg.device_name));
    }
    if (json_obj_get(root, "favorite_teams", &fv)) {
        json_as_str(fv, cfg.favorite_teams, sizeof(cfg.favorite_teams));
        sanitize_teams_csv(cfg.favorite_teams);
    }
    if (json_obj_get(root, "api_base_url", &fv)) {
        json_as_str(fv, cfg.api_base_url, sizeof(cfg.api_base_url));
        trim_url(cfg.api_base_url);
    }
    if (json_obj_get(root, "timezone", &fv)) {
        json_as_str(fv, cfg.timezone, sizeof(cfg.timezone));
    }
    if (json_obj_get(root, "ntp_server", &fv)) {
        json_as_str(fv, cfg.ntp_server, sizeof(cfg.ntp_server));
    }
    if (json_obj_get(root, "quiet_enabled", &fv)) {
        json_as_bool(fv, &cfg.quiet_enabled);
    }
    if (json_obj_get(root, "quiet_start", &fv)) {
        json_as_str(fv, cfg.quiet_start, sizeof(cfg.quiet_start));
    }
    if (json_obj_get(root, "quiet_end", &fv)) {
        json_as_str(fv, cfg.quiet_end, sizeof(cfg.quiet_end));
    }
    int64_t iv = 0;
    if (json_obj_get(root, "brightness_override", &fv) && json_as_int(fv, &iv)) {
        cfg.brightness_override = (iv >= 0 && iv <= 255) ? (int)iv : -1;
    }
    if (json_obj_get(root, "brightness_min", &fv) && json_as_int(fv, &iv) && iv >= 1 && iv <= 255) {
        cfg.brightness_min = (uint8_t)iv;
    }
    if (json_obj_get(root, "brightness_max", &fv) && json_as_int(fv, &iv) && iv >= 1 && iv <= 255) {
        cfg.brightness_max = (uint8_t)iv;
    }
    if (cfg.brightness_max < cfg.brightness_min) {
        cfg.brightness_max = cfg.brightness_min;
    }
    if (json_obj_get(root, "df_volume", &fv) && json_as_int(fv, &iv) && iv >= 0 && iv <= 30) {
        cfg.df_volume = (uint8_t)iv;
    }
    if (json_obj_get(root, "df_eq", &fv) && json_as_int(fv, &iv) && iv >= 0 && iv <= 5) {
        cfg.df_eq = (uint8_t)iv;
    }
    if (json_obj_get(root, "boot_tone", &fv)) {
        json_as_bool(fv, &cfg.boot_tone);
    }
    if (json_obj_get(root, "hourly_chime", &fv)) {
        json_as_bool(fv, &cfg.hourly_chime);
    }
    if (json_obj_get(root, "rotary_idle_sec", &fv) && json_as_int(fv, &iv) && iv >= 0 && iv <= 600) {
        cfg.rotary_idle_sec = (uint16_t)iv;
    }
    if (json_obj_get(root, "poll_interval_sec", &fv) && json_as_int(fv, &iv) && iv >= 1 && iv <= 3600) {
        cfg.poll_interval_sec = (uint16_t)iv;
    }
    if (json_obj_get(root, "ota_enabled", &fv)) {
        json_as_bool(fv, &cfg.ota_enabled);
    }
    if (json_obj_get(root, "fight_folder", &fv) && json_as_int(fv, &iv) && iv >= 0 && iv <= 99) {
        cfg.fight_folder = (uint8_t)iv;
    }
    if (json_obj_get(root, "fight_track", &fv) && json_as_int(fv, &iv) && iv >= 1 && iv <= 2999) {
        cfg.fight_track = (uint16_t)iv;
    }
    if (json_obj_get(root, "victory_folder", &fv) && json_as_int(fv, &iv) && iv >= 0 && iv <= 99) {
        cfg.victory_folder = (uint8_t)iv;
    }
    if (json_obj_get(root, "victory_track", &fv) && json_as_int(fv, &iv) && iv >= 1 && iv <= 2999) {
        cfg.victory_track = (uint16_t)iv;
    }
    if (json_obj_get(root, "third_folder", &fv) && json_as_int(fv, &iv) && iv >= 0 && iv <= 99) {
        cfg.third_folder = (uint8_t)iv;
    }
    if (json_obj_get(root, "third_track", &fv) && json_as_int(fv, &iv) && iv >= 1 && iv <= 2999) {
        cfg.third_track = (uint16_t)iv;
    }

    char pinned_app[32] = "";
    bool have_pinned = false;
    if (json_obj_get(root, "pinned_app", &fv)) {
        have_pinned = json_as_str(fv, pinned_app, sizeof(pinned_app));
        if (have_pinned) {
            snprintf(cfg.pinned_app, sizeof(cfg.pinned_app), "%s", pinned_app);
        }
    }

    char *app_settings_raw = NULL;
    if (json_obj_get(root, "app_settings", &fv) && !json_is_null(fv) && fv.len >= 2) {
        app_settings_raw = malloc(fv.len + 1U);
        if (app_settings_raw) {
            memcpy(app_settings_raw, fv.ptr, fv.len);
            app_settings_raw[fv.len] = '\0';
        }
    }
    free(body);

    bool cloud_synced = false;
    if (wifi_manager_is_connected()) {
        cloud_synced = (api_client_post_config(&cfg, NULL, app_settings_raw) == ESP_OK);
    }
    free(app_settings_raw);

    cfg.cloud_sync_pending = !cloud_synced;
    err = settings_store_save(&cfg);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed saving NVS");
        return err;
    }

    if (s_hooks.on_settings_updated) {
        s_hooks.on_settings_updated(&cfg, have_pinned ? pinned_app : NULL);
    }
    if (s_hooks.on_request_refresh) {
        s_hooks.on_request_refresh();
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, cloud_synced
                                       ? "{\"ok\":true,\"cloud_synced\":true}"
                                       : "{\"ok\":true,\"cloud_synced\":false}");
}

static esp_err_t handle_get_apps(httpd_req_t *req)
{
    if (!wifi_manager_is_connected()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"error\":\"offline\"}");
    }
    char *json = NULL;
    size_t len = 0;
    esp_err_t err = api_client_fetch_apps_json(&json, &len);
    if (err != ESP_OK || !json) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "502 Bad Gateway");
        return httpd_resp_sendstr(req, "{\"error\":\"server_unreachable\"}");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t res = httpd_resp_send(req, json, (ssize_t)len);
    free(json);
    return res;
}

static esp_err_t handle_get_cloud_config(httpd_req_t *req)
{
    if (!wifi_manager_is_connected()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"error\":\"offline\"}");
    }
    char *json = NULL;
    size_t len = 0;
    esp_err_t err = api_client_fetch_device_config_json(&json, &len);
    if (err != ESP_OK || !json) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "502 Bad Gateway");
        return httpd_resp_sendstr(req, "{\"error\":\"server_unreachable\"}");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t res = httpd_resp_send(req, json, (ssize_t)len);
    free(json);
    return res;
}

static esp_err_t handle_get_frame(httpd_req_t *req)
{
    uint8_t *fb = malloc(CS_FRAME_RGB888_BYTES);
    if (!fb) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = hub75_get_frame_rgb888(fb);
    if (err != ESP_OK) {
        free(fb);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "Display not ready");
        return err;
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t res = httpd_resp_send(req, (const char *)fb, CS_FRAME_RGB888_BYTES);
    free(fb);
    return res;
}

static esp_err_t handle_post_action(httpd_req_t *req)
{
    char *body = NULL;
    size_t body_len = 0;
    esp_err_t err = read_req_body(req, 256, &body, &body_len);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid action payload");
        return err;
    }

    char action[32] = {0};
    char app_arg[32] = {0};
    int64_t vol_arg = -1;
    int64_t bright_arg = -2;
    json_span_t root = json_root(body, body_len);
    json_span_t fv;
    if (json_obj_get(root, "action", &fv)) {
        json_as_str(fv, action, sizeof(action));
    }
    if (json_obj_get(root, "app_id", &fv)) {
        json_as_str(fv, app_arg, sizeof(app_arg));
    }
    if (json_obj_get(root, "volume", &fv)) {
        json_as_int(fv, &vol_arg);
    }
    if (json_obj_get(root, "brightness", &fv)) {
        json_as_int(fv, &bright_arg);
    }
    free(body);

    if (strcmp(action, "refresh") == 0) {
        if (s_hooks.on_request_refresh) {
            s_hooks.on_request_refresh();
        }
    } else if (strcmp(action, "check_ota") == 0) {
        if (s_hooks.on_request_ota) {
            s_hooks.on_request_ota();
        }
    } else if (strcmp(action, "set_app") == 0) {
        cs_local_settings_t cfg;
        settings_store_get(&cfg);
        snprintf(cfg.pinned_app, sizeof(cfg.pinned_app), "%s", app_arg);
        settings_store_save(&cfg);
        if (s_hooks.on_set_pinned_app) {
            s_hooks.on_set_pinned_app(app_arg);
        }
    } else if (strcmp(action, "set_brightness") == 0 && bright_arg >= -1 && bright_arg <= 255) {
        cs_local_settings_t cfg;
        settings_store_get(&cfg);
        cfg.brightness_override = (int)bright_arg;
        settings_store_save(&cfg);
        if (s_hooks.on_apply_brightness) {
            s_hooks.on_apply_brightness(cfg.brightness_override);
        }
    } else if (strcmp(action, "set_volume") == 0 && vol_arg >= 0 && vol_arg <= 30) {
        cs_local_settings_t cfg;
        settings_store_get(&cfg);
        cfg.df_volume = (uint8_t)vol_arg;
        settings_store_save(&cfg);
        if (s_hooks.on_audio_action) {
            s_hooks.on_audio_action("apply_volume");
        }
    } else if (strcmp(action, "play_fight_song") == 0 ||
               strcmp(action, "play_victory_song") == 0 ||
               strcmp(action, "play_third_song") == 0 ||
               strcmp(action, "test_boot_tone") == 0 ||
               strcmp(action, "test_chime") == 0 ||
               strcmp(action, "stop_audio") == 0) {
        if (s_hooks.on_audio_action) {
            s_hooks.on_audio_action(action);
        }
    } else if (strcmp(action, "reboot") == 0) {
        schedule_deferred(DEFER_REBOOT, 400);
    } else if (strcmp(action, "reset_wifi") == 0) {
        settings_store_clear_wifi();
        schedule_deferred(DEFER_ENTER_SETUP, 400);
    } else if (strcmp(action, "factory_reset") == 0) {
        schedule_deferred(DEFER_FACTORY_RESET, 400);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown action");
        return ESP_ERR_INVALID_ARG;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t handle_post_test_server(httpd_req_t *req)
{
    char test_url[192] = {0};
    if (req->content_len > 0) {
        char *body = NULL;
        size_t body_len = 0;
        if (read_req_body(req, 512, &body, &body_len) == ESP_OK && body) {
            json_span_t root = json_root(body, body_len);
            json_span_t fv;
            if (json_obj_get(root, "api_base_url", &fv)) {
                json_as_str(fv, test_url, sizeof(test_url));
            }
            free(body);
        }
    }

    cs_server_diag_t *diag = calloc(1, sizeof(cs_server_diag_t));
    char *out = malloc(2560);
    if (!diag || !out) {
        free(diag);
        free(out);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_ERR_NO_MEM;
    }

    api_client_diagnose_server(test_url[0] ? test_url : NULL, diag);

    size_t cap = 2560;
    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, cap - pos, "{\"url\":");
    pos = append_json_str(out, cap, pos, diag->url);
    pos += (size_t)snprintf(out + pos, cap - pos,
                            ",\"url_valid\":%s,\"is_https\":%s,\"host\":",
                            diag->url_valid ? "true" : "false",
                            diag->is_https ? "true" : "false");
    pos = append_json_str(out, cap, pos, diag->host);
    pos += (size_t)snprintf(out + pos, cap - pos,
                            ",\"port\":%u,\"wifi_connected\":%s,\"wifi_ssid\":",
                            (unsigned)diag->port,
                            diag->wifi_connected ? "true" : "false");
    pos = append_json_str(out, cap, pos, diag->wifi_ssid);
    pos += (size_t)snprintf(out + pos, cap - pos, ",\"ip\":");
    pos = append_json_str(out, cap, pos, diag->ip);
    pos += (size_t)snprintf(out + pos, cap - pos, ",\"dns_server\":");
    pos = append_json_str(out, cap, pos, diag->dns_server);
    pos += (size_t)snprintf(out + pos, cap - pos,
                            ",\"rssi\":%d,\"dns_ok\":%s,\"resolved_ip\":",
                            diag->rssi, diag->dns_ok ? "true" : "false");
    pos = append_json_str(out, cap, pos, diag->resolved_ip);
    pos += (size_t)snprintf(out + pos, cap - pos,
                            ",\"dns_ms\":%lu,\"dns_err\":%d,\"apps_ok\":%s,"
                            "\"apps_status\":%d,\"apps_err\":",
                            (unsigned long)diag->dns_ms,
                            diag->dns_err,
                            diag->apps_ok ? "true" : "false",
                            diag->apps_status);
    pos = append_json_str(out, cap, pos, esp_err_to_name(diag->apps_err));
    pos += (size_t)snprintf(out + pos, cap - pos,
                            ",\"apps_ms\":%lu,\"tls_err\":%d,\"cert_flags\":%lu,"
                            "\"sock_errno\":%d,\"redirect_location\":",
                            (unsigned long)diag->apps_ms,
                            diag->tls_err,
                            (unsigned long)diag->cert_flags,
                            diag->sock_errno);
    pos = append_json_str(out, cap, pos, diag->redirect_location);
    pos += (size_t)snprintf(out + pos, cap - pos,
                            ",\"config_ok\":%s,\"config_status\":%d,\"config_err\":",
                            diag->config_ok ? "true" : "false",
                            diag->config_status);
    pos = append_json_str(out, cap, pos, esp_err_to_name(diag->config_err));
    pos += (size_t)snprintf(out + pos, cap - pos, ",\"config_ms\":%lu,\"stage\":",
                            (unsigned long)diag->config_ms);
    pos = append_json_str(out, cap, pos, diag->stage);
    pos += (size_t)snprintf(out + pos, cap - pos, ",\"hint\":");
    pos = append_json_str(out, cap, pos, diag->hint);
    pos += (size_t)snprintf(out + pos, cap - pos, ",\"ok\":%s}",
                            (diag->apps_ok && diag->config_ok) ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t res = httpd_resp_send(req, out, (ssize_t)strlen(out));
    free(out);
    free(diag);
    return res;
}

esp_err_t web_portal_start(const web_portal_hooks_t *hooks)
{
    if (s_server) {
        return ESP_OK;
    }
    if (hooks) {
        s_hooks = *hooks;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 12288;
    config.max_uri_handlers = 14;
    config.lru_purge_enable = true;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "httpd_start failed");

    const httpd_uri_t routes[] = {
        { .uri = "/",                       .method = HTTP_GET,  .handler = handle_root },
        { .uri = "/api/local/status",       .method = HTTP_GET,  .handler = handle_get_status },
        { .uri = "/api/local/frame",        .method = HTTP_GET,  .handler = handle_get_frame },
        { .uri = "/api/local/scan",         .method = HTTP_GET,  .handler = handle_get_scan },
        { .uri = "/api/local/setup",        .method = HTTP_POST, .handler = handle_post_setup },
        { .uri = "/api/local/settings",     .method = HTTP_POST, .handler = handle_post_settings },
        { .uri = "/api/local/apps",         .method = HTTP_GET,  .handler = handle_get_apps },
        { .uri = "/api/local/cloud-config", .method = HTTP_GET,  .handler = handle_get_cloud_config },
        { .uri = "/api/local/action",       .method = HTTP_POST, .handler = handle_post_action },
        { .uri = "/api/local/test-server",  .method = HTTP_POST, .handler = handle_post_test_server },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &routes[i]),
                            TAG, "register route failed");
    }
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, handle_captive_404);

    ESP_LOGI(TAG, "Local web portal started on port %u", config.server_port);
    return ESP_OK;
}
