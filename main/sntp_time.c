#include "sntp_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"

static const char *TAG = "sntp_time";

typedef struct {
    const char *iana;
    const char *posix_tz;
} tz_map_entry_t;

static const tz_map_entry_t s_tz_map[] = {
    {"America/New_York",             "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Detroit",              "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Indiana/Indianapolis", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Chicago",              "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Denver",               "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Phoenix",              "MST7"},
    {"America/Los_Angeles",          "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Anchorage",            "AKST9AKDT,M3.2.0,M11.1.0"},
    {"Pacific/Honolulu",             "HST10"},
    {"America/Puerto_Rico",          "AST4"},
    {"America/Toronto",              "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Vancouver",            "PST8PDT,M3.2.0,M11.1.0"},
    {"Europe/London",                "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Paris",                 "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Berlin",                "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Amsterdam",             "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"UTC",                          "UTC0"},
    {"Etc/UTC",                      "UTC0"},
};

static bool s_sntp_started = false;
static char s_current_tz[64] = {0};
static char s_ntp_server[64] = CONFIG_CS_NTP_SERVER;

static const char *resolve_posix_tz(const char *iana_or_posix)
{
    if (!iana_or_posix || !*iana_or_posix) {
        return "CST6CDT,M3.2.0,M11.1.0";
    }
    for (size_t i = 0; i < sizeof(s_tz_map) / sizeof(s_tz_map[0]); i++) {
        if (strcasecmp(iana_or_posix, s_tz_map[i].iana) == 0) {
            return s_tz_map[i].posix_tz;
        }
    }
    // If it has no '/' it is likely already a POSIX TZ string (e.g. "CST6CDT")
    if (strchr(iana_or_posix, '/') == NULL) {
        return iana_or_posix;
    }
    ESP_LOGW(TAG, "Unknown IANA timezone '%s', falling back to UTC0", iana_or_posix);
    return "UTC0";
}

void sntp_time_set_timezone(const char *iana_or_posix_tz)
{
    const char *posix_tz = resolve_posix_tz(iana_or_posix_tz);
    if (strcmp(s_current_tz, posix_tz) == 0) {
        return;
    }
    snprintf(s_current_tz, sizeof(s_current_tz), "%s", posix_tz);
    setenv("TZ", s_current_tz, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set to %s (%s)",
             iana_or_posix_tz ? iana_or_posix_tz : "", s_current_tz);
}

void sntp_time_set_server(const char *ntp_server)
{
    const char *srv = (ntp_server && *ntp_server) ? ntp_server : CONFIG_CS_NTP_SERVER;
    if (strcmp(s_ntp_server, srv) == 0) {
        return;
    }
    snprintf(s_ntp_server, sizeof(s_ntp_server), "%s", srv);
    if (s_sntp_started) {
        esp_netif_sntp_deinit();
        s_sntp_started = false;
        sntp_time_init();
    }
}

void sntp_time_init(void)
{
    if (s_sntp_started) {
        return;
    }
    if (!s_current_tz[0]) {
        sntp_time_set_timezone(CONFIG_CS_DEFAULT_TIMEZONE);
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_ntp_server);
    cfg.wait_for_sync = false;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err == ESP_OK) {
        s_sntp_started = true;
        ESP_LOGI(TAG, "SNTP started (server=%s)", s_ntp_server);
    } else {
        ESP_LOGW(TAG, "esp_netif_sntp_init failed: %s", esp_err_to_name(err));
    }
}

bool sntp_time_is_synced(void)
{
    time_t now = time(NULL);
    struct tm tm_info = {0};
    localtime_r(&now, &tm_info);
    return tm_info.tm_year >= (2024 - 1900);
}

void sntp_time_format_clock(const char *fmt, char *out, size_t out_cap)
{
    if (!out || out_cap == 0) {
        return;
    }
    if (!sntp_time_is_synced()) {
        snprintf(out, out_cap, "--:--");
        return;
    }
    time_t now = time(NULL);
    struct tm tm_info = {0};
    localtime_r(&now, &tm_info);

    const char *f = (fmt && *fmt) ? fmt : "%I:%M";
    bool is_24 = (strstr(f, "%H") != NULL);
    bool has_sec = (strstr(f, "%S") != NULL);
    bool has_ampm = (strstr(f, "%p") != NULL);
    bool has_now_prefix = (strncmp(f, "NOW ", 4) == 0);

    if (is_24) {
        if (has_sec) {
            snprintf(out, out_cap, "%s%02d:%02d:%02d",
                     has_now_prefix ? "NOW " : "",
                     tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
        } else {
            snprintf(out, out_cap, "%s%02d:%02d",
                     has_now_prefix ? "NOW " : "",
                     tm_info.tm_hour, tm_info.tm_min);
        }
    } else {
        int h12 = tm_info.tm_hour % 12;
        if (h12 == 0) h12 = 12;
        const char *ampm = (tm_info.tm_hour >= 12) ? "PM" : "AM";
        if (has_sec) {
            snprintf(out, out_cap, "%s%d:%02d:%02d%s%s",
                     has_now_prefix ? "NOW " : "",
                     h12, tm_info.tm_min, tm_info.tm_sec,
                     has_ampm ? " " : "", has_ampm ? ampm : "");
        } else {
            snprintf(out, out_cap, "%s%d:%02d%s%s",
                     has_now_prefix ? "NOW " : "",
                     h12, tm_info.tm_min,
                     has_ampm ? " " : "", has_ampm ? ampm : "");
        }
    }
}

void sntp_time_format_date(char *out, size_t out_cap)
{
    static const char *s_days[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    static const char *s_mons[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                     "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    if (!out || out_cap == 0) {
        return;
    }
    if (!sntp_time_is_synced()) {
        snprintf(out, out_cap, "SYNCING");
        return;
    }
    time_t now = time(NULL);
    struct tm tm_info = {0};
    localtime_r(&now, &tm_info);
    int wday = (tm_info.tm_wday >= 0 && tm_info.tm_wday < 7) ? tm_info.tm_wday : 0;
    int mon = (tm_info.tm_mon >= 0 && tm_info.tm_mon < 12) ? tm_info.tm_mon : 0;
    snprintf(out, out_cap, "%s %s %02d", s_days[wday], s_mons[mon], tm_info.tm_mday);
}

void sntp_time_format_countdown(int64_t target_epoch, char *out, size_t out_cap)
{
    if (!out || out_cap == 0) {
        return;
    }
    if (!sntp_time_is_synced() || target_epoch <= 0) {
        snprintf(out, out_cap, "PREGAME");
        return;
    }
    long rem = (long)target_epoch - (long)time(NULL);
    if (rem <= 0) {
        snprintf(out, out_cap, "KICKOFF SOON");
        return;
    }
    long d = rem / 86400;
    long h = (rem % 86400) / 3600;
    long m = (rem % 3600) / 60;
    long s = rem % 60;

    if (d > 0) {
        snprintf(out, out_cap, "KICK %ldD %02ldH %02ldM", d, h, m);
    } else if (h > 0) {
        snprintf(out, out_cap, "KICK %02ldH %02ldM %02ldS", h, m, s);
    } else {
        snprintf(out, out_cap, "KICK %02ldM %02ldS", m, s);
    }
}

static int parse_hhmm(const char *hhmm)
{
    if (!hhmm || strlen(hhmm) < 5) {
        return -1;
    }
    int hh = atoi(hhmm);
    int mm = atoi(hhmm + 3);
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) {
        return -1;
    }
    return hh * 60 + mm;
}

bool sntp_time_in_quiet_hours(bool enabled, const char *start_hhmm, const char *end_hhmm)
{
    if (!enabled || !sntp_time_is_synced()) {
        return false;
    }
    int start_m = parse_hhmm(start_hhmm);
    int end_m = parse_hhmm(end_hhmm);
    if (start_m < 0 || end_m < 0 || start_m == end_m) {
        return false;
    }
    time_t now = time(NULL);
    struct tm tm_info = {0};
    localtime_r(&now, &tm_info);
    int cur_m = tm_info.tm_hour * 60 + tm_info.tm_min;
    if (start_m < end_m) {
        return cur_m >= start_m && cur_m < end_m;
    }
    return cur_m >= start_m || cur_m < end_m;
}
