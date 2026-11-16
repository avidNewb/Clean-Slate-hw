#include "api_client.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"
#include "miniz.h"

#include "json_min.h"
#include "settings_store.h"
#include "wifi_manager.h"

static const char *TAG = "api_client";

#define MAX_HTTP_BODY_BYTES (196608)
#define OTA_REBOOT_MAGIC    (0x43534F54UL) /* "CSOT" */
#define CS_HTTP_MIN_TIMEOUT_MS (15000)

static RTC_NOINIT_ATTR uint32_t s_ota_reboot_magic;
static portMUX_TYPE s_diag_mux = portMUX_INITIALIZER_UNLOCKED;
static cs_api_last_status_t s_last_status = {
    .stage = "idle",
    .last_ok_age_sec = -1,
};
static int64_t s_last_ok_us = 0;

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

static void *psram_prefer_realloc(void *ptr, size_t size)
{
    return heap_caps_realloc_prefer(ptr, size, 2,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static size_t append_url_encoded(char *dst, size_t cap, size_t pos, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!s || pos >= cap) {
        return pos;
    }
    for (size_t i = 0; s[i] != '\0'; i++) {
        unsigned char c = (unsigned char)s[i];
        bool safe = (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~');
        if (safe) {
            if (pos + 1 >= cap) break;
            dst[pos++] = (char)c;
        } else {
            if (pos + 3 >= cap) break;
            dst[pos++] = '%';
            dst[pos++] = hex[(c >> 4) & 0x0F];
            dst[pos++] = hex[c & 0x0F];
        }
    }
    if (pos < cap) {
        dst[pos] = '\0';
    }
    return pos;
}

struct cs_strip_stream {
    const uint8_t *in_buf;
    size_t in_len;
    size_t in_pos;
    bool is_zlib;
    bool done;
    uint16_t frames_emitted;
    uint16_t frame_limit;
    tinfl_decompressor *decomp;
    uint8_t *dict;
    size_t dict_ofs;
    size_t dict_read_ofs;
    size_t dict_avail;
};

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

static void parse_hex_color(const char *s, uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = 255;
    *g = 255;
    *b = 255;
    if (!s || !*s) {
        return;
    }
    if (*s == '#') {
        s++;
    }
    if (strlen(s) < 6) {
        return;
    }
    int r0 = hex_nibble(s[0]), r1 = hex_nibble(s[1]);
    int g0 = hex_nibble(s[2]), g1 = hex_nibble(s[3]);
    int b0 = hex_nibble(s[4]), b1 = hex_nibble(s[5]);
    if (r0 < 0 || r1 < 0 || g0 < 0 || g1 < 0 || b0 < 0 || b1 < 0) {
        return;
    }
    *r = (uint8_t)((r0 << 4) | r1);
    *g = (uint8_t)((g0 << 4) | g1);
    *b = (uint8_t)((b0 << 4) | b1);
}

static void parse_json_color(json_span_t obj, uint8_t *r, uint8_t *g, uint8_t *b)
{
    char hex[16] = {0};
    json_span_t fv;
    if ((json_obj_get(obj, "color_hex", &fv) || json_obj_get(obj, "color", &fv)) &&
        json_as_str(fv, hex, sizeof(hex))) {
        parse_hex_color(hex, r, g, b);
        return;
    }
    int64_t iv = 0;
    if (json_obj_get(obj, "color_rgb565", &fv) && json_as_int(fv, &iv)) {
        gfx_rgb565_to_rgb888((uint16_t)iv, r, g, b);
    }
}

static gfx_font_id_t parse_font_id(const char *s)
{
    if (s) {
        if (strcmp(s, "4x6") == 0) {
            return GFX_FONT_4X6;
        }
        if (strcmp(s, "5x7") == 0) {
            return GFX_FONT_5X7;
        }
    }
    return GFX_FONT_3X5;
}

static gfx_align_t parse_align(const char *s)
{
    if (s) {
        if (strcmp(s, "center") == 0) {
            return GFX_ALIGN_CENTER;
        }
        if (strcmp(s, "right") == 0) {
            return GFX_ALIGN_RIGHT;
        }
    }
    return GFX_ALIGN_LEFT;
}

static cs_overlay_type_t parse_overlay_type(const char *s)
{
    if (!s) {
        return CS_OVERLAY_NONE;
    }
    if (strcmp(s, "ntp_time") == 0) {
        return CS_OVERLAY_NTP_TIME;
    }
    if (strcmp(s, "ntp_date") == 0) {
        return CS_OVERLAY_NTP_DATE;
    }
    if (strcmp(s, "countdown") == 0) {
        return CS_OVERLAY_COUNTDOWN;
    }
    if (strcmp(s, "text") == 0) {
        return CS_OVERLAY_TEXT;
    }
    return CS_OVERLAY_NONE;
}

typedef struct {
    int tls_err;
    uint32_t cert_flags;
    int sock_errno;
    char redirect_loc[192];
} http_diag_ctx_t;

static void extract_endpoint_label(const char *url, char *out, size_t cap)
{
    if (!out || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (!url || !*url) {
        return;
    }
    const char *p = strstr(url, "://");
    p = p ? strchr(p + 3, '/') : strchr(url, '/');
    if (!p) {
        snprintf(out, cap, "/");
        return;
    }
    snprintf(out, cap, "%s", p);
}

static esp_err_t http_diag_event_handler(esp_http_client_event_t *evt)
{
    http_diag_ctx_t *ctx = (http_diag_ctx_t *)evt->user_data;
    if (!ctx) {
        return ESP_OK;
    }
    if (evt->event_id == HTTP_EVENT_ERROR && evt->data) {
        esp_tls_error_handle_t tls_err = (esp_tls_error_handle_t)evt->data;
        int mbed_err = 0;
        int sock_err = 0;
        int last_esp = esp_tls_get_and_clear_last_error(tls_err, &mbed_err, &sock_err);
        if (mbed_err != 0) {
            ctx->tls_err = mbed_err;
        } else if (last_esp != 0 && ctx->tls_err == 0) {
            ctx->tls_err = last_esp;
        }
        uint32_t flags = (uint32_t)sock_err;
        if (flags != 0U) {
            ctx->cert_flags = flags;
        }
        int e = errno;
        if (e != 0) {
            ctx->sock_errno = e;
        }
    } else if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value) {
        if (strcasecmp(evt->header_key, "location") == 0) {
            snprintf(ctx->redirect_loc, sizeof(ctx->redirect_loc), "%s", evt->header_value);
        }
    }
    return ESP_OK;
}

static void record_api_telemetry(const char *url,
                                 esp_err_t err,
                                 int status,
                                 const http_diag_ctx_t *ctx,
                                 uint32_t latency_ms,
                                 const char *stage)
{
    int64_t now_us = esp_timer_get_time();
    bool ok = (err == ESP_OK && status >= 200 && status < 300);

    char ep[48];
    extract_endpoint_label(url, ep, sizeof(ep));

    char detail[128];
    if (ok) {
        snprintf(detail, sizeof(detail), "HTTP %d in %lums", status, (unsigned long)latency_ms);
    } else if (status >= 300 && status < 400) {
        snprintf(detail, sizeof(detail), "HTTP %d redirect%s%.72s",
                 status,
                 (ctx && ctx->redirect_loc[0]) ? " -> " : "",
                 (ctx && ctx->redirect_loc[0]) ? ctx->redirect_loc : "");
    } else if (status > 0) {
        snprintf(detail, sizeof(detail), "HTTP %d (%.48s)", status, esp_err_to_name(err));
    } else if (ctx && ctx->cert_flags != 0) {
        snprintf(detail, sizeof(detail), "TLS cert verify failed (-0x%X, flags=0x%lX)",
                 (unsigned)(-ctx->tls_err), (unsigned long)ctx->cert_flags);
    } else if (ctx && ctx->tls_err != 0) {
        snprintf(detail, sizeof(detail), "TLS error -0x%X (errno=%d)",
                 (unsigned)(-ctx->tls_err), ctx->sock_errno);
    } else if (ctx && ctx->sock_errno != 0) {
        snprintf(detail, sizeof(detail), "%.40s (errno=%d: %.48s)",
                 esp_err_to_name(err), ctx->sock_errno, strerror(ctx->sock_errno));
    } else {
        snprintf(detail, sizeof(detail), "%.96s", esp_err_to_name(err));
    }

    portENTER_CRITICAL(&s_diag_mux);
    s_last_status.attempted = true;
    s_last_status.ok = ok;
    s_last_status.http_status = status;
    s_last_status.err = err;
    s_last_status.tls_err = ctx ? ctx->tls_err : 0;
    s_last_status.cert_flags = ctx ? ctx->cert_flags : 0U;
    s_last_status.sock_errno = ctx ? ctx->sock_errno : 0;
    s_last_status.latency_ms = latency_ms;
    snprintf(s_last_status.endpoint, sizeof(s_last_status.endpoint), "%s", ep);
    snprintf(s_last_status.stage, sizeof(s_last_status.stage), "%s", stage ? stage : (ok ? "ok" : "error"));
    snprintf(s_last_status.detail, sizeof(s_last_status.detail), "%s", detail);
    if (ok) {
        s_last_ok_us = now_us;
    }
    portEXIT_CRITICAL(&s_diag_mux);
}

static esp_err_t http_request_alloc_once(esp_http_client_method_t method,
                                         const char *url,
                                         const char *post_body,
                                         size_t post_len,
                                         char **out_buf,
                                         size_t *out_len,
                                         int *out_status,
                                         http_diag_ctx_t *diag_out,
                                         const char **stage_out)
{
    *out_buf = NULL;
    *out_len = 0;
    if (out_status) {
        *out_status = 0;
    }
    if (diag_out) {
        memset(diag_out, 0, sizeof(*diag_out));
    }
    if (stage_out) {
        *stage_out = "init";
    }

    int timeout_ms = CONFIG_CS_API_HTTP_TIMEOUT_MS;
    if (timeout_ms < CS_HTTP_MIN_TIMEOUT_MS) {
        timeout_ms = CS_HTTP_MIN_TIMEOUT_MS;
    }

    http_diag_ctx_t local_ctx = {0};
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = timeout_ms,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_diag_event_handler,
        .user_data = &local_ctx,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        if (stage_out) *stage_out = "oom";
        return ESP_ERR_NO_MEM;
    }

    if (post_body && post_len > 0) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
    }

    if (stage_out) *stage_out = "connect";
    esp_err_t err = esp_http_client_open(client, (post_body && post_len > 0) ? (int)post_len : 0);
    if (err != ESP_OK) {
        if (local_ctx.tls_err != 0 || local_ctx.cert_flags != 0) {
            if (stage_out) *stage_out = "tls";
        }
        if (local_ctx.sock_errno == 0 && errno != 0) {
            local_ctx.sock_errno = errno;
        }
        ESP_LOGW(TAG, "HTTP open failed for %s: err=%s tls=-0x%X flags=0x%lX errno=%d",
                 url, esp_err_to_name(err),
                 (unsigned)(-local_ctx.tls_err),
                 (unsigned long)local_ctx.cert_flags,
                 local_ctx.sock_errno);
        if (diag_out) {
            *diag_out = local_ctx;
        }
        esp_http_client_cleanup(client);
        return err;
    }

    if (post_body && post_len > 0) {
        if (stage_out) *stage_out = "write";
        int written = esp_http_client_write(client, post_body, (int)post_len);
        if (written < 0) {
            if (local_ctx.sock_errno == 0 && errno != 0) {
                local_ctx.sock_errno = errno;
            }
            if (diag_out) *diag_out = local_ctx;
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
    }

    if (stage_out) *stage_out = "headers";
    int64_t content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (out_status) {
        *out_status = status;
    }
    if (diag_out) {
        *diag_out = local_ctx;
    }

    if (content_len > MAX_HTTP_BODY_BYTES) {
        ESP_LOGW(TAG, "HTTP body too large (%lld bytes) for %s", (long long)content_len, url);
        if (stage_out) *stage_out = "body";
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    size_t cap = (content_len > 0) ? ((size_t)content_len + 1U) : 8192U;
    if (cap > MAX_HTTP_BODY_BYTES + 1U) {
        cap = MAX_HTTP_BODY_BYTES + 1U;
    }

    char *buf = psram_prefer_malloc(cap);
    if (!buf) {
        if (stage_out) *stage_out = "oom";
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    if (stage_out) *stage_out = "body";
    size_t total = 0;
    while (true) {
        if (total + 1U >= cap) {
            if (cap >= MAX_HTTP_BODY_BYTES + 1U) {
                err = ESP_ERR_NO_MEM;
                break;
            }
            size_t new_cap = cap * 2U;
            if (new_cap > MAX_HTTP_BODY_BYTES + 1U) {
                new_cap = MAX_HTTP_BODY_BYTES + 1U;
            }
            char *grown = psram_prefer_realloc(buf, new_cap);
            if (!grown) {
                err = ESP_ERR_NO_MEM;
                break;
            }
            buf = grown;
            cap = new_cap;
        }

        int r = esp_http_client_read(client, buf + total, (int)(cap - 1U - total));
        if (r < 0) {
            err = ESP_FAIL;
            break;
        }
        if (r == 0) {
            if (esp_http_client_is_complete_data_received(client) || content_len < 0) {
                err = ESP_OK;
                break;
            }
        } else {
            total += (size_t)r;
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        free(buf);
        return err;
    }

    buf[total] = '\0';
    *out_buf = buf;
    *out_len = total;
    if (stage_out) {
        *stage_out = (status >= 200 && status < 300) ? "ok" : "http";
    }
    return (status >= 200 && status < 300) ? ESP_OK : ESP_FAIL;
}

static esp_err_t http_request_alloc_ex(esp_http_client_method_t method,
                                       const char *url,
                                       const char *post_body,
                                       size_t post_len,
                                       char **out_buf,
                                       size_t *out_len,
                                       int *out_status,
                                       http_diag_ctx_t *diag_out,
                                       uint32_t *latency_ms_out,
                                       bool record_telemetry)
{
    int64_t t0 = esp_timer_get_time();
    char cur_url[384];
    snprintf(cur_url, sizeof(cur_url), "%s", url ? url : "");

    http_diag_ctx_t ctx = {0};
    const char *stage = "init";
    int status = 0;
    esp_err_t err = ESP_FAIL;

    for (int hop = 0; hop < 3; hop++) {
        err = http_request_alloc_once(method, cur_url, post_body, post_len,
                                      out_buf, out_len, &status, &ctx, &stage);
        bool is_redirect = (method == HTTP_METHOD_GET) &&
                           (status == 301 || status == 302 || status == 303 ||
                            status == 307 || status == 308) &&
                           (strncmp(ctx.redirect_loc, "http://", 7) == 0 ||
                            strncmp(ctx.redirect_loc, "https://", 8) == 0);
        if (!is_redirect || hop == 2) {
            break;
        }
        ESP_LOGI(TAG, "Following HTTP %d redirect -> %s", status, ctx.redirect_loc);
        free(*out_buf);
        *out_buf = NULL;
        *out_len = 0;
        snprintf(cur_url, sizeof(cur_url), "%s", ctx.redirect_loc);
    }

    uint32_t lat_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000LL);
    if (out_status) {
        *out_status = status;
    }
    if (diag_out) {
        *diag_out = ctx;
    }
    if (latency_ms_out) {
        *latency_ms_out = lat_ms;
    }
    if (record_telemetry) {
        record_api_telemetry(url, err, status, &ctx, lat_ms, stage);
    }
    return err;
}

static esp_err_t http_request_alloc(esp_http_client_method_t method,
                                    const char *url,
                                    const char *post_body,
                                    size_t post_len,
                                    char **out_buf,
                                    size_t *out_len,
                                    int *out_status)
{
    return http_request_alloc_ex(method, url, post_body, post_len,
                                 out_buf, out_len, out_status, NULL, NULL, true);
}

static size_t append_json_escaped(char *dst, size_t cap, size_t pos, const char *s)
{
    if (!s) {
        return pos;
    }
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
        } else if (c == '\t') {
            dst[pos++] = '\\';
            dst[pos++] = 't';
        } else if (c >= 0x20) {
            dst[pos++] = (char)c;
        }
    }
    if (pos < cap) {
        dst[pos] = '\0';
    }
    return pos;
}

static size_t append_csv_array(char *dst, size_t cap, size_t pos, const char *csv)
{
    pos += (size_t)snprintf(dst + pos, (pos < cap) ? (cap - pos) : 0, "[");
    if (csv && *csv) {
        char copy[128];
        snprintf(copy, sizeof(copy), "%s", csv);
        char *saveptr = NULL;
        char *tok = strtok_r(copy, ",", &saveptr);
        bool first = true;
        while (tok && pos + 8 < cap) {
            while (*tok == ' ') tok++;
            size_t len = strlen(tok);
            while (len > 0 && tok[len - 1] == ' ') tok[--len] = '\0';
            if (len > 0) {
                if (!first) {
                    pos += (size_t)snprintf(dst + pos, cap - pos, ",");
                }
                pos += (size_t)snprintf(dst + pos, cap - pos, "\"");
                pos = append_json_escaped(dst, cap, pos, tok);
                pos += (size_t)snprintf(dst + pos, (pos < cap) ? (cap - pos) : 0, "\"");
                first = false;
            }
            tok = strtok_r(NULL, ",", &saveptr);
        }
    }
    pos += (size_t)snprintf(dst + pos, (pos < cap) ? (cap - pos) : 0, "]");
    return pos;
}

esp_err_t api_client_fetch_config(cs_device_config_t *out_cfg)
{
    if (!out_cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_cfg, 0, sizeof(*out_cfg));
    out_cfg->brightness_override = -1;
    snprintf(out_cfg->device_name, sizeof(out_cfg->device_name), "Clean Slate");
    snprintf(out_cfg->quiet_start, sizeof(out_cfg->quiet_start), "23:00");
    snprintf(out_cfg->quiet_end, sizeof(out_cfg->quiet_end), "07:00");
    snprintf(out_cfg->timezone, sizeof(out_cfg->timezone), "%s", CONFIG_CS_DEFAULT_TIMEZONE);

    char base[192];
    settings_store_get_api_base_url(base, sizeof(base));
    char url[288];
    snprintf(url, sizeof(url), "%s/api/v1/device/%s/config",
             base, wifi_manager_get_device_id());

    char *body = NULL;
    size_t body_len = 0;
    int status = 0;
    esp_err_t err = http_request_alloc(HTTP_METHOD_GET, url, NULL, 0, &body, &body_len, &status);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GET /config failed (status=%d, err=%s)", status, esp_err_to_name(err));
        free(body);
        return err;
    }

    json_span_t root = json_root(body, body_len);
    json_span_t val;

    if (json_obj_get(root, "device_name", &val)) {
        json_as_str(val, out_cfg->device_name, sizeof(out_cfg->device_name));
    }

    if (json_obj_get(root, "favorite_teams", &val)) {
        size_t cur = 0;
        json_span_t elem;
        size_t pos = 0;
        while (json_arr_iter(val, &cur, &elem)) {
            char t[16];
            if (json_as_str(elem, t, sizeof(t)) && t[0]) {
                if (pos > 0 && pos + 1 < sizeof(out_cfg->favorite_teams)) {
                    out_cfg->favorite_teams[pos++] = ',';
                }
                pos += (size_t)snprintf(out_cfg->favorite_teams + pos,
                                        sizeof(out_cfg->favorite_teams) - pos, "%s", t);
            }
        }
    }

    if (json_obj_get(root, "enabled_apps", &val)) {
        size_t cur = 0;
        json_span_t elem;
        while (out_cfg->enabled_app_count < CS_MAX_APPS && json_arr_iter(val, &cur, &elem)) {
            if (json_as_str(elem, out_cfg->enabled_apps[out_cfg->enabled_app_count],
                            sizeof(out_cfg->enabled_apps[0]))) {
                out_cfg->enabled_app_count++;
            }
        }
    }

    if (json_obj_get(root, "quiet_hours", &val) && !json_is_null(val)) {
        json_span_t qv;
        if (json_obj_get(val, "enabled", &qv)) {
            json_as_bool(qv, &out_cfg->quiet_enabled);
        }
        if (json_obj_get(val, "start", &qv)) {
            json_as_str(qv, out_cfg->quiet_start, sizeof(out_cfg->quiet_start));
        }
        if (json_obj_get(val, "end", &qv)) {
            json_as_str(qv, out_cfg->quiet_end, sizeof(out_cfg->quiet_end));
        }
        if (json_obj_get(val, "timezone", &qv)) {
            json_as_str(qv, out_cfg->timezone, sizeof(out_cfg->timezone));
        }
    }

    if (json_obj_get(root, "brightness_override", &val) && !json_is_null(val)) {
        int64_t bval = -1;
        if (json_as_int(val, &bval) && bval >= 0 && bval <= 255) {
            out_cfg->brightness_override = (int)bval;
        }
    }

    out_cfg->valid = true;
    free(body);
    return ESP_OK;
}

esp_err_t api_client_post_config(const cs_local_settings_t *settings,
                                 const char *enabled_apps_csv,
                                 const char *app_settings_json_obj)
{
    if (!settings) {
        return ESP_ERR_INVALID_ARG;
    }

    char base[192];
    settings_store_get_api_base_url(base, sizeof(base));
    char url[288];
    snprintf(url, sizeof(url), "%s/api/v1/device/%s/config",
             base, wifi_manager_get_device_id());

    size_t extra_len = app_settings_json_obj ? strlen(app_settings_json_obj) : 0;
    size_t cap = 1024 + extra_len;
    char *payload = malloc(cap);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    size_t pos = 0;
    pos += (size_t)snprintf(payload + pos, cap - pos, "{\"device_name\":\"");
    pos = append_json_escaped(payload, cap, pos, settings->device_name);
    pos += (size_t)snprintf(payload + pos, cap - pos, "\",\"favorite_teams\":");
    pos = append_csv_array(payload, cap, pos, settings->favorite_teams);

    if (enabled_apps_csv && *enabled_apps_csv) {
        pos += (size_t)snprintf(payload + pos, cap - pos, ",\"enabled_apps\":");
        pos = append_csv_array(payload, cap, pos, enabled_apps_csv);
    }

    pos += (size_t)snprintf(payload + pos, cap - pos,
                            ",\"quiet_hours\":{\"enabled\":%s,\"start\":\"",
                            settings->quiet_enabled ? "true" : "false");
    pos = append_json_escaped(payload, cap, pos, settings->quiet_start);
    pos += (size_t)snprintf(payload + pos, cap - pos, "\",\"end\":\"");
    pos = append_json_escaped(payload, cap, pos, settings->quiet_end);
    pos += (size_t)snprintf(payload + pos, cap - pos, "\",\"timezone\":\"");
    pos = append_json_escaped(payload, cap, pos, settings->timezone);
    pos += (size_t)snprintf(payload + pos, cap - pos, "\"}");

    if (settings->brightness_override >= 0 && settings->brightness_override <= 255) {
        pos += (size_t)snprintf(payload + pos, cap - pos,
                                ",\"brightness_override\":%d", settings->brightness_override);
    } else {
        pos += (size_t)snprintf(payload + pos, cap - pos, ",\"brightness_override\":null");
    }

    if (app_settings_json_obj && app_settings_json_obj[0] == '{') {
        pos += (size_t)snprintf(payload + pos, cap - pos,
                                ",\"app_settings\":%s", app_settings_json_obj);
    }
    pos += (size_t)snprintf(payload + pos, (pos < cap) ? (cap - pos) : 0, "}");

    char *resp = NULL;
    size_t resp_len = 0;
    int status = 0;
    esp_err_t err = http_request_alloc(HTTP_METHOD_POST, url, payload, pos,
                                       &resp, &resp_len, &status);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "POST /config failed (status=%d, err=%s)", status, esp_err_to_name(err));
    }
    free(payload);
    free(resp);
    return err;
}

esp_err_t api_client_fetch_apps_json(char **out_json, size_t *out_len)
{
    if (!out_json || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    char base[192];
    settings_store_get_api_base_url(base, sizeof(base));
    char url[256];
    snprintf(url, sizeof(url), "%s/api/v1/apps", base);
    int status = 0;
    esp_err_t err = http_request_alloc(HTTP_METHOD_GET, url, NULL, 0, out_json, out_len, &status);
    if (err != ESP_OK) {
        free(*out_json);
        *out_json = NULL;
        *out_len = 0;
    }
    return err;
}

esp_err_t api_client_fetch_device_config_json(char **out_json, size_t *out_len)
{
    if (!out_json || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    char base[192];
    settings_store_get_api_base_url(base, sizeof(base));
    char url[288];
    snprintf(url, sizeof(url), "%s/api/v1/device/%s/config",
             base, wifi_manager_get_device_id());
    int status = 0;
    esp_err_t err = http_request_alloc(HTTP_METHOD_GET, url, NULL, 0, out_json, out_len, &status);
    if (err != ESP_OK) {
        free(*out_json);
        *out_json = NULL;
        *out_len = 0;
    }
    return err;
}

static void parse_overlays(json_span_t overlays_arr, cs_frame_response_t *out)
{
    size_t cur = 0;
    json_span_t item;
    while (out->overlay_count < CS_MAX_OVERLAYS && json_arr_iter(overlays_arr, &cur, &item)) {
        cs_overlay_t ov;
        memset(&ov, 0, sizeof(ov));
        ov.r = 255;
        ov.g = 255;
        ov.b = 255;
        ov.font = GFX_FONT_3X5;
        ov.align = GFX_ALIGN_LEFT;

        char tmp[64];
        json_span_t fv;
        if (!json_obj_get(item, "type", &fv) || !json_as_str(fv, tmp, sizeof(tmp))) {
            continue;
        }
        ov.type = parse_overlay_type(tmp);
        if (ov.type == CS_OVERLAY_NONE) {
            continue;
        }

        if (json_obj_get(item, "id", &fv) && !json_is_null(fv)) {
            json_as_str(fv, ov.id, sizeof(ov.id));
        }
        int64_t iv = 0;
        if (json_obj_get(item, "x", &fv) && json_as_int(fv, &iv)) {
            ov.x = (int16_t)iv;
        }
        if (json_obj_get(item, "y", &fv) && json_as_int(fv, &iv)) {
            ov.y = (int16_t)iv;
        }
        if (json_obj_get(item, "font", &fv) && json_as_str(fv, tmp, sizeof(tmp))) {
            ov.font = parse_font_id(tmp);
        }
        if (json_obj_get(item, "align", &fv) && json_as_str(fv, tmp, sizeof(tmp))) {
            ov.align = parse_align(tmp);
        }
        if (json_obj_get(item, "color_rgb565", &fv) && !json_is_null(fv) && json_as_int(fv, &iv)) {
            ov.color_rgb565 = (uint16_t)iv;
        }
        parse_json_color(item, &ov.r, &ov.g, &ov.b);
        if (json_obj_get(item, "format", &fv)) {
            json_as_str(fv, ov.format, sizeof(ov.format));
        }
        if (json_obj_get(item, "text", &fv)) {
            json_as_str(fv, ov.text, sizeof(ov.text));
        }
        if (json_obj_get(item, "target_epoch", &fv) && !json_is_null(fv)) {
            json_as_int(fv, &ov.target_epoch);
        }

        out->overlays[out->overlay_count++] = ov;
    }
}

static void parse_alert(json_span_t alert_obj, cs_alert_t *out)
{
    memset(out, 0, sizeof(*out));
    out->marquee_y = 26;
    out->marquee_r = 255;
    out->marquee_g = 255;
    out->marquee_b = 255;
    out->frame_duration_ms = 125;

    json_span_t fv;
    if (json_obj_get(alert_obj, "event_type", &fv)) {
        json_as_str(fv, out->event_type, sizeof(out->event_type));
    }

    json_span_t strip;
    if (!json_obj_get(alert_obj, "sprite_strip", &strip) || json_is_null(strip)) {
        return;
    }

    int64_t iv = 0;
    if (json_obj_get(strip, "frame_count", &fv) && json_as_int(fv, &iv) && iv > 0) {
        out->frame_count = (uint16_t)(iv > CS_MAX_ALERT_FRAMES ? CS_MAX_ALERT_FRAMES : iv);
    }
    if (json_obj_get(strip, "frame_duration_ms", &fv) && json_as_int(fv, &iv) && iv >= 20) {
        out->frame_duration_ms = (uint16_t)iv;
    }

    bool explicit_zlib = false;
    bool explicit_raw = false;
    char comp_str[16] = {0};
    if (json_obj_get(strip, "compression", &fv) && json_as_str(fv, comp_str, sizeof(comp_str))) {
        if (strcmp(comp_str, "zlib") == 0) {
            explicit_zlib = true;
        } else if (strcmp(comp_str, "none") == 0 || strcmp(comp_str, "raw") == 0) {
            explicit_raw = true;
        }
    } else if (json_obj_get(strip, "compressed", &fv)) {
        bool c = false;
        if (json_as_bool(fv, &c)) {
            explicit_zlib = c;
            explicit_raw = !c;
        }
    }

    const char *b64_ptr = NULL;
    size_t b64_len = 0;
    if (!json_obj_get(strip, "data_b64", &fv) || !json_str_inner(fv, &b64_ptr, &b64_len) ||
        b64_len == 0) {
        return;
    }

    size_t max_raw = (b64_len * 3U) / 4U + 4U;
    uint8_t *decoded = psram_prefer_malloc(max_raw);
    if (!decoded) {
        ESP_LOGW(TAG, "Failed allocating %u bytes for alert strip (free_heap=%lu, largest_block=%lu)",
                 (unsigned)max_raw,
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return;
    }

    size_t decoded_len = 0;
    int rc = mbedtls_base64_decode(decoded, max_raw, &decoded_len,
                                   (const unsigned char *)b64_ptr, b64_len);
    if (rc != 0 || decoded_len == 0) {
        ESP_LOGW(TAG, "Alert base64 decode failed (rc=%d)", rc);
        free(decoded);
        return;
    }

    out->strip_data = decoded;
    out->strip_data_len = decoded_len;
    if (explicit_zlib) {
        out->strip_is_zlib = true;
    } else if (explicit_raw) {
        out->strip_is_zlib = false;
    } else {
        out->strip_is_zlib = (decoded_len >= 2 &&
                              decoded[0] == 0x78 &&
                              ((((uint16_t)decoded[0] << 8) | decoded[1]) % 31U) == 0);
    }

    json_span_t mq;
    if (json_obj_get(alert_obj, "marquee", &mq) && !json_is_null(mq)) {
        if (json_obj_get(mq, "text", &fv)) {
            json_as_str(fv, out->marquee_text, sizeof(out->marquee_text));
        }
        if (json_obj_get(mq, "y", &fv) && json_as_int(fv, &iv)) {
            out->marquee_y = (int16_t)iv;
        }
        parse_json_color(mq, &out->marquee_r, &out->marquee_g, &out->marquee_b);
    }

    json_span_t audio;
    if (json_obj_get(alert_obj, "audio", &audio) && !json_is_null(audio)) {
        out->has_audio = true;
        out->audio_folder = 1;
        out->audio_track = 1;
        out->audio_volume = CONFIG_CS_DFPLAYER_DEFAULT_VOLUME;
        if (json_obj_get(audio, "folder", &fv) && json_as_int(fv, &iv) && iv > 0) {
            out->audio_folder = (uint8_t)iv;
        }
        if (json_obj_get(audio, "track", &fv) && json_as_int(fv, &iv) && iv > 0) {
            out->audio_track = (uint8_t)iv;
        }
        if (json_obj_get(audio, "volume", &fv) && json_as_int(fv, &iv) && iv >= 0 && iv <= 30) {
            out->audio_volume = (uint8_t)iv;
        }
    }

    out->valid = (out->frame_count > 0 && out->strip_data != NULL);
    if (!out->valid) {
        ESP_LOGW(TAG, "Alert discarded (event=%s, frames=%u, strip_len=%u)",
                 out->event_type[0] ? out->event_type : "unknown",
                 (unsigned)out->frame_count,
                 (unsigned)out->strip_data_len);
        api_client_free_alert(out);
    } else {
        ESP_LOGI(TAG, "Parsed alert: event=%s frames=%u dur=%ums strip=%uB (%s) marquee=%s audio=%s",
                 out->event_type[0] ? out->event_type : "alert",
                 (unsigned)out->frame_count,
                 (unsigned)out->frame_duration_ms,
                 (unsigned)out->strip_data_len,
                 out->strip_is_zlib ? "zlib" : "raw",
                 out->marquee_text[0] ? out->marquee_text : "none",
                 out->has_audio ? "yes" : "no");
    }
}

esp_err_t api_client_fetch_frame(const char *app_id_override,
                                 const char *teams_override,
                                 cs_frame_response_t *out_frame)
{
    if (!out_frame) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_frame, 0, sizeof(*out_frame));
    out_frame->refresh_seconds = CONFIG_CS_API_POLL_INTERVAL_SEC;

    char base[192];
    settings_store_get_api_base_url(base, sizeof(base));

    char url[384];
    int n = snprintf(url, sizeof(url), "%s/api/v1/device/%s/frame",
                     base, wifi_manager_get_device_id());
    size_t pos = (n > 0) ? (size_t)n : 0U;
    if (pos > sizeof(url)) {
        pos = sizeof(url);
    }
    bool has_q = false;
    if (app_id_override && *app_id_override && pos + 9 < sizeof(url)) {
        pos += (size_t)snprintf(url + pos, sizeof(url) - pos, "?app_id=");
        pos = append_url_encoded(url, sizeof(url), pos, app_id_override);
        has_q = true;
    }
    if (teams_override && *teams_override && pos + 8 < sizeof(url)) {
        pos += (size_t)snprintf(url + pos, sizeof(url) - pos, "%steams=", has_q ? "&" : "?");
        append_url_encoded(url, sizeof(url), pos, teams_override);
    }

    char *body = NULL;
    size_t body_len = 0;
    int status = 0;
    esp_err_t err = http_request_alloc(HTTP_METHOD_GET, url, NULL, 0, &body, &body_len, &status);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GET /frame failed (status=%d, err=%s)", status, esp_err_to_name(err));
        free(body);
        return err;
    }

    json_span_t root = json_root(body, body_len);
    json_span_t val;

    if (json_obj_get(root, "app_id", &val)) {
        json_as_str(val, out_frame->app_id, sizeof(out_frame->app_id));
    }
    int64_t rsec = 0;
    if (json_obj_get(root, "refresh_seconds", &val) && json_as_int(val, &rsec) && rsec >= 1) {
        out_frame->refresh_seconds = (uint32_t)rsec;
    }
    if (json_obj_get(root, "quiet_mode", &val)) {
        json_as_bool(val, &out_frame->quiet_mode);
    }

    const char *b64_ptr = NULL;
    size_t b64_len = 0;
    if (!json_obj_get(root, "frame_b64", &val) || !json_str_inner(val, &b64_ptr, &b64_len) ||
        b64_len == 0) {
        ESP_LOGW(TAG, "Missing frame_b64 in /frame response");
        free(body);
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t *rgb565be = psram_prefer_malloc(CS_FRAME_RGB565_BYTES + 8);
    if (!rgb565be) {
        free(body);
        return ESP_ERR_NO_MEM;
    }

    size_t dec_len = 0;
    int rc = mbedtls_base64_decode(rgb565be, CS_FRAME_RGB565_BYTES + 8, &dec_len,
                                   (const unsigned char *)b64_ptr, b64_len);
    if (rc != 0 || dec_len != CS_FRAME_RGB565_BYTES) {
        ESP_LOGW(TAG, "frame_b64 decode failed (rc=%d, len=%u)", rc, (unsigned)dec_len);
        free(rgb565be);
        free(body);
        return ESP_ERR_INVALID_RESPONSE;
    }

    gfx_rgb565be_to_rgb888(rgb565be, CS_FRAME_PIXELS, out_frame->bg_rgb888);
    free(rgb565be);

    if (json_obj_get(root, "overlays", &val) && !json_is_null(val)) {
        parse_overlays(val, out_frame);
    }

    if (json_obj_get(root, "alert", &val) && !json_is_null(val)) {
        parse_alert(val, &out_frame->alert);
    }

    free(body);
    return ESP_OK;
}

void api_client_free_alert(cs_alert_t *alert)
{
    if (!alert) {
        return;
    }
    free(alert->strip_data);
    memset(alert, 0, sizeof(*alert));
}

/* Cached decompression state lives in PSRAM (with internal fallback on non-PSRAM boards)
 * instead of consuming ~44 KB in static internal .dram0.bss. */
static cs_strip_stream_t s_static_stream;
static tinfl_decompressor *s_cached_decomp = NULL;
static uint8_t *s_cached_dict = NULL;
static bool s_static_stream_busy = false;

cs_strip_stream_t *api_client_strip_stream_begin(const cs_alert_t *alert)
{
    if (!alert || !alert->valid || !alert->strip_data || alert->frame_count == 0) {
        return NULL;
    }

    cs_strip_stream_t *s = NULL;
    bool using_static = false;
    if (!s_static_stream_busy) {
        s_static_stream_busy = true;
        using_static = true;
        s = &s_static_stream;
        memset(s, 0, sizeof(*s));
    } else {
        s = psram_prefer_calloc(1, sizeof(*s));
        if (!s) {
            return NULL;
        }
    }

    s->in_buf = alert->strip_data;
    s->in_len = alert->strip_data_len;
    s->is_zlib = alert->strip_is_zlib;
    s->frame_limit = alert->frame_count;

    if (s->is_zlib) {
        if (using_static) {
            if (!s_cached_decomp) {
                s_cached_decomp = psram_prefer_malloc(sizeof(tinfl_decompressor));
            }
            if (!s_cached_dict) {
                s_cached_dict = psram_prefer_malloc(TINFL_LZ_DICT_SIZE);
            }
            if (!s_cached_decomp || !s_cached_dict) {
                api_client_strip_stream_end(s);
                return NULL;
            }
            s->decomp = s_cached_decomp;
            s->dict = s_cached_dict;
        } else {
            s->decomp = psram_prefer_malloc(sizeof(tinfl_decompressor));
            s->dict = psram_prefer_malloc(TINFL_LZ_DICT_SIZE);
            if (!s->decomp || !s->dict) {
                api_client_strip_stream_end(s);
                return NULL;
            }
        }
        tinfl_init(s->decomp);
    }

    return s;
}

bool api_client_strip_stream_next(cs_strip_stream_t *stream, uint8_t *out_rgb565be_4096)
{
    if (!stream || !out_rgb565be_4096 || stream->frames_emitted >= stream->frame_limit) {
        return false;
    }

    if (!stream->is_zlib) {
        if (stream->done || stream->in_pos + CS_FRAME_RGB565_BYTES > stream->in_len) {
            stream->done = true;
            return false;
        }
        memcpy(out_rgb565be_4096, stream->in_buf + stream->in_pos, CS_FRAME_RGB565_BYTES);
        stream->in_pos += CS_FRAME_RGB565_BYTES;
        stream->frames_emitted++;
        return true;
    }

    size_t out_filled = 0;
    while (out_filled < CS_FRAME_RGB565_BYTES) {
        if (stream->dict_read_ofs < stream->dict_avail) {
            size_t buffered = stream->dict_avail - stream->dict_read_ofs;
            size_t take = CS_FRAME_RGB565_BYTES - out_filled;
            if (take > buffered) {
                take = buffered;
            }
            memcpy(out_rgb565be_4096 + out_filled,
                   stream->dict + stream->dict_read_ofs,
                   take);
            stream->dict_read_ofs += take;
            out_filled += take;
            continue;
        }

        if (stream->done) {
            break;
        }

        /* miniz requires (pOut_buf_next - pOut_buf_start) + *pOut_buf_size to be a power of 2
         * when wrapping; filling to the end of the 32 KiB dictionary keeps that sum == 32768. */
        if (stream->dict_ofs >= TINFL_LZ_DICT_SIZE) {
            stream->dict_ofs = 0;
        }
        size_t in_avail = stream->in_len - stream->in_pos;
        size_t out_bytes = TINFL_LZ_DICT_SIZE - stream->dict_ofs;

        tinfl_status st = tinfl_decompress(
            stream->decomp,
            stream->in_buf + stream->in_pos, &in_avail,
            stream->dict, stream->dict + stream->dict_ofs, &out_bytes,
            TINFL_FLAG_PARSE_ZLIB_HEADER);

        stream->in_pos += in_avail;

        if (st < 0) {
            ESP_LOGW(TAG, "tinfl_decompress failed (status=%d, frame=%u, in_pos=%u/%u)",
                     (int)st, (unsigned)stream->frames_emitted,
                     (unsigned)stream->in_pos, (unsigned)stream->in_len);
            stream->done = true;
            return false;
        }

        if (out_bytes > 0) {
            stream->dict_read_ofs = stream->dict_ofs;
            stream->dict_avail = stream->dict_ofs + out_bytes;
            stream->dict_ofs = (stream->dict_ofs + out_bytes) & (TINFL_LZ_DICT_SIZE - 1U);
        }

        if (st == TINFL_STATUS_DONE) {
            stream->done = true;
            if (out_bytes == 0) {
                break;
            }
        } else if (in_avail == 0 && out_bytes == 0) {
            stream->done = true;
            break;
        }
    }

    if (out_filled != CS_FRAME_RGB565_BYTES) {
        ESP_LOGW(TAG, "Short decompressed frame %u (%u/%u bytes)",
                 (unsigned)stream->frames_emitted,
                 (unsigned)out_filled,
                 (unsigned)CS_FRAME_RGB565_BYTES);
        return false;
    }
    stream->frames_emitted++;
    return true;
}

void api_client_strip_stream_end(cs_strip_stream_t *stream)
{
    if (!stream) {
        return;
    }
    if (stream == &s_static_stream) {
        memset(&s_static_stream, 0, sizeof(s_static_stream));
        s_static_stream_busy = false;
        return;
    }
    free(stream->decomp);
    free(stream->dict);
    free(stream);
}

bool api_client_consume_ota_reboot_flag(void)
{
    bool is_ota = (esp_reset_reason() == ESP_RST_SW && s_ota_reboot_magic == OTA_REBOOT_MAGIC);
    s_ota_reboot_magic = 0;
    return is_ota;
}

esp_err_t api_client_check_and_run_ota(const char *current_version,
                                       bool *update_avail_out,
                                       char *detail_out,
                                       size_t detail_cap)
{
    if (update_avail_out) {
        *update_avail_out = false;
    }
    if (detail_out && detail_cap > 0) {
        detail_out[0] = '\0';
    }
#if !CONFIG_CS_OTA_ENABLED
    (void)current_version;
    if (detail_out && detail_cap > 0) {
        snprintf(detail_out, detail_cap, "OTA disabled in firmware build");
    }
    return ESP_OK;
#else
    if (!current_version || !*current_version) {
        return ESP_ERR_INVALID_ARG;
    }

    char base[192];
    settings_store_get_api_base_url(base, sizeof(base));
    char url[288];
    snprintf(url, sizeof(url), "%s/api/v1/firmware/check?current_version=%s",
             base, current_version);

    char *body = NULL;
    size_t body_len = 0;
    int status = 0;
    esp_err_t err = http_request_alloc(HTTP_METHOD_GET, url, NULL, 0, &body, &body_len, &status);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTA check failed (status=%d, err=%s)", status, esp_err_to_name(err));
        if (detail_out && detail_cap > 0) {
            snprintf(detail_out, detail_cap, "HTTP %d (%s)", status, esp_err_to_name(err));
        }
        free(body);
        return err;
    }

    json_span_t root = json_root(body, body_len);
    json_span_t fv;
    bool update_avail = false;
    if (json_obj_get(root, "update_available", &fv)) {
        json_as_bool(fv, &update_avail);
    }
    if (update_avail_out) {
        *update_avail_out = update_avail;
    }

    char latest_ver[32] = {0};
    if (json_obj_get(root, "latest_version", &fv)) {
        json_as_str(fv, latest_ver, sizeof(latest_ver));
    }

    if (!update_avail) {
        if (detail_out && detail_cap > 0) {
            snprintf(detail_out, detail_cap, "%s", latest_ver[0] ? latest_ver : current_version);
        }
        free(body);
        return ESP_OK;
    }

    char dl_path[192] = {0};
    if (json_obj_get(root, "download_url", &fv)) {
        json_as_str(fv, dl_path, sizeof(dl_path));
    }
    if (!dl_path[0]) {
        snprintf(dl_path, sizeof(dl_path), "/api/v1/firmware/download");
    }
    free(body);

    char ota_url[392];
    if (strncmp(dl_path, "http://", 7) == 0 || strncmp(dl_path, "https://", 8) == 0) {
        snprintf(ota_url, sizeof(ota_url), "%s", dl_path);
    } else {
        snprintf(ota_url, sizeof(ota_url), "%s%s%s",
                 base,
                 (dl_path[0] == '/') ? "" : "/",
                 dl_path);
    }

    ESP_LOGI(TAG, "OTA update available (%s -> %s), downloading %s",
             current_version, latest_ver, ota_url);
    if (detail_out && detail_cap > 0) {
        snprintf(detail_out, detail_cap, "%s", latest_ver[0] ? latest_ver : "updating");
    }

    esp_http_client_config_t http_cfg = {
        .url = ota_url,
        .timeout_ms = 20000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    err = esp_https_ota(&ota_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA update succeeded, restarting...");
        s_ota_reboot_magic = OTA_REBOOT_MAGIC;
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA update failed: %s", esp_err_to_name(err));
        if (detail_out && detail_cap > 0) {
            snprintf(detail_out, detail_cap, "%s", esp_err_to_name(err));
        }
    }
    return err;
#endif
}

void api_client_get_last_status(cs_api_last_status_t *out)
{
    if (!out) {
        return;
    }
    int64_t ok_us = 0;
    portENTER_CRITICAL(&s_diag_mux);
    *out = s_last_status;
    ok_us = s_last_ok_us;
    portEXIT_CRITICAL(&s_diag_mux);

    if (ok_us > 0) {
        int64_t age_s = (esp_timer_get_time() - ok_us) / 1000000LL;
        out->last_ok_age_sec = (age_s < 0) ? 0 : (int)age_s;
    } else {
        out->last_ok_age_sec = -1;
    }
}

static void normalize_diag_url(const char *raw, char *out, size_t cap)
{
    if (!out || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (!raw) {
        return;
    }
    while (*raw && isspace((unsigned char)*raw)) {
        raw++;
    }
    snprintf(out, cap, "%s", raw);
    size_t n = strlen(out);
    while (n > 0 && (isspace((unsigned char)out[n - 1]) || out[n - 1] == '/')) {
        out[--n] = '\0';
    }
}

static bool parse_base_url(const char *url, bool *is_https_out, char *host_out, size_t host_cap, uint16_t *port_out)
{
    if (!url || !*url || !is_https_out || !host_out || host_cap == 0 || !port_out) {
        return false;
    }
    host_out[0] = '\0';
    const char *p = NULL;
    if (strncmp(url, "https://", 8) == 0) {
        *is_https_out = true;
        *port_out = 443;
        p = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        *is_https_out = false;
        *port_out = 80;
        p = url + 7;
    } else {
        return false;
    }

    if (*p == '\0' || *p == '/') {
        return false;
    }

    const char *end = p;
    while (*end && *end != '/' && *end != ':' && *end != '?' && *end != '#') {
        end++;
    }
    size_t hlen = (size_t)(end - p);
    if (hlen == 0 || hlen >= host_cap) {
        return false;
    }
    memcpy(host_out, p, hlen);
    host_out[hlen] = '\0';

    if (*end == ':') {
        unsigned long port_val = strtoul(end + 1, NULL, 10);
        if (port_val == 0 || port_val > 65535) {
            return false;
        }
        *port_out = (uint16_t)port_val;
    }
    return true;
}

esp_err_t api_client_diagnose_server(const char *override_base_url, cs_server_diag_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    if (override_base_url && *override_base_url) {
        normalize_diag_url(override_base_url, out->url, sizeof(out->url));
    } else {
        settings_store_get_api_base_url(out->url, sizeof(out->url));
        normalize_diag_url(out->url, out->url, sizeof(out->url));
    }

    out->wifi_connected = wifi_manager_is_connected();
    wifi_manager_get_sta_ssid(out->wifi_ssid, sizeof(out->wifi_ssid));
    wifi_manager_get_ip_str(out->ip, sizeof(out->ip));
    wifi_manager_get_dns_str(out->dns_server, sizeof(out->dns_server));
    out->rssi = wifi_manager_get_rssi();

    out->url_valid = parse_base_url(out->url, &out->is_https, out->host, sizeof(out->host), &out->port);
    if (!out->url_valid) {
        snprintf(out->stage, sizeof(out->stage), "url");
        snprintf(out->hint, sizeof(out->hint),
                 "Server URL must start with http:// or https:// and include a valid hostname.");
        return ESP_ERR_INVALID_ARG;
    }

    if (!out->wifi_connected) {
        snprintf(out->stage, sizeof(out->stage), "wifi");
        snprintf(out->hint, sizeof(out->hint),
                 "Device Wi-Fi is not connected. Connect to your 2.4 GHz Wi-Fi network before testing the server.");
        return ESP_ERR_INVALID_STATE;
    }

    int64_t dns_t0 = esp_timer_get_time();
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)out->port);
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    int gai_rc = getaddrinfo(out->host, port_str, &hints, &res);
    out->dns_ms = (uint32_t)((esp_timer_get_time() - dns_t0) / 1000LL);
    if (gai_rc == 0 && res) {
        out->dns_ok = true;
        if (res->ai_addr && res->ai_addr->sa_family == AF_INET) {
            const struct sockaddr_in *sa = (const struct sockaddr_in *)res->ai_addr;
            inet_ntop(AF_INET, &sa->sin_addr, out->resolved_ip, sizeof(out->resolved_ip));
        }
        freeaddrinfo(res);
    } else {
        out->dns_ok = false;
        out->dns_err = gai_rc;
        if (res) {
            freeaddrinfo(res);
        }
        snprintf(out->stage, sizeof(out->stage), "dns");
        snprintf(out->hint, sizeof(out->hint),
                 "DNS lookup failed for %.80s (code %d, DNS=%.16s). Check hostname spelling, router internet connection, or DNS.",
                 out->host, gai_rc, out->dns_server[0] ? out->dns_server : "unknown");
        return ESP_FAIL;
    }

    char apps_url[256];
    snprintf(apps_url, sizeof(apps_url), "%s/api/v1/apps", out->url);
    char *body = NULL;
    size_t body_len = 0;
    http_diag_ctx_t diag_ctx = {0};
    esp_err_t err = http_request_alloc_ex(HTTP_METHOD_GET, apps_url, NULL, 0,
                                          &body, &body_len, &out->apps_status,
                                          &diag_ctx, &out->apps_ms, false);
    free(body);
    body = NULL;

    out->apps_err = err;
    out->apps_ok = (err == ESP_OK && out->apps_status >= 200 && out->apps_status < 300);
    out->tls_err = diag_ctx.tls_err;
    out->cert_flags = diag_ctx.cert_flags;
    out->sock_errno = diag_ctx.sock_errno;
    snprintf(out->redirect_location, sizeof(out->redirect_location), "%s", diag_ctx.redirect_loc);

    if (!out->apps_ok) {
        if (out->redirect_location[0] && out->apps_status >= 300 && out->apps_status < 400) {
            snprintf(out->stage, sizeof(out->stage), "http");
            snprintf(out->hint, sizeof(out->hint),
                     "Server redirected (HTTP %d) to %.128s. Update the Server URL to match cleanly.",
                     out->apps_status, out->redirect_location);
        } else if (out->cert_flags != 0 ||
                   (out->tls_err <= -0x2000 && out->tls_err >= -0x3000)) {
            snprintf(out->stage, sizeof(out->stage), "tls");
            snprintf(out->hint, sizeof(out->hint),
                     "TLS certificate check failed for %.80s (-0x%X, flags=0x%lX). Verify the exact Azure hostname and CA bundle.",
                     out->host, (unsigned)(-out->tls_err), (unsigned long)out->cert_flags);
        } else if (out->tls_err != 0) {
            snprintf(out->stage, sizeof(out->stage), "tls");
            snprintf(out->hint, sizeof(out->hint),
                     "TLS handshake failed with %.80s (%.32s): mbedTLS -0x%X, errno=%d. Azure may still be warming up.",
                     out->host, out->resolved_ip, (unsigned)(-out->tls_err), out->sock_errno);
        } else if (out->apps_status >= 400) {
            snprintf(out->stage, sizeof(out->stage), "http");
            snprintf(out->hint, sizeof(out->hint),
                     "Connected to %.80s (%.32s), but GET /api/v1/apps returned HTTP %d. Confirm this is the Clean Slate backend.",
                     out->host, out->resolved_ip, out->apps_status);
        } else {
            snprintf(out->stage, sizeof(out->stage), "connect");
            snprintf(out->hint, sizeof(out->hint),
                     "TCP/HTTP request to %.80s (%.32s) failed: %.48s (errno=%d: %.64s).",
                     out->host, out->resolved_ip, esp_err_to_name(err),
                     out->sock_errno, out->sock_errno ? strerror(out->sock_errno) : "timeout/reset");
        }
        return err != ESP_OK ? err : ESP_FAIL;
    }

    char cfg_url[288];
    snprintf(cfg_url, sizeof(cfg_url), "%s/api/v1/device/%s/config",
             out->url, wifi_manager_get_device_id());
    http_diag_ctx_t cfg_ctx = {0};
    err = http_request_alloc_ex(HTTP_METHOD_GET, cfg_url, NULL, 0,
                                &body, &body_len, &out->config_status,
                                &cfg_ctx, &out->config_ms, false);
    free(body);
    out->config_err = err;
    out->config_ok = (err == ESP_OK && out->config_status >= 200 && out->config_status < 300);

    if (!out->config_ok) {
        snprintf(out->stage, sizeof(out->stage), "config");
        snprintf(out->hint, sizeof(out->hint),
                 "/api/v1/apps works (HTTP %d), but /api/v1/device/%s/config returned HTTP %d (%s).",
                 out->apps_status, wifi_manager_get_device_id(),
                 out->config_status, esp_err_to_name(err));
        return err != ESP_OK ? err : ESP_FAIL;
    }

    snprintf(out->stage, sizeof(out->stage), "ok");
    snprintf(out->hint, sizeof(out->hint),
             "Connected! DNS (%lums -> %s), /api/v1/apps (HTTP %d, %lums), and device /config (HTTP %d, %lums) passed.",
             (unsigned long)out->dns_ms, out->resolved_ip,
             out->apps_status, (unsigned long)out->apps_ms,
             out->config_status, (unsigned long)out->config_ms);
    return ESP_OK;
}
