#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize SNTP client and set default timezone from Kconfig.
 */
void sntp_time_init(void);

/**
 * Update SNTP server hostname/IP (defaults to CONFIG_CS_NTP_SERVER).
 */
void sntp_time_set_server(const char *ntp_server);

/**
 * Set local timezone from an IANA name (e.g. "America/Chicago") or POSIX TZ string.
 */
void sntp_time_set_timezone(const char *iana_or_posix_tz);

/**
 * Return true once system wall-clock time has been synchronized via SNTP.
 */
bool sntp_time_is_synced(void);

/**
 * Format local time overlay matching Clean Slate preview_ui.py / clock_app.py.
 */
void sntp_time_format_clock(const char *fmt, char *out, size_t out_cap);

/**
 * Format local date overlay (e.g. "SUN NOV 14").
 */
void sntp_time_format_date(char *out, size_t out_cap);

/**
 * Format countdown to target_epoch using prefix from fmt (e.g. "TIP ..." or "KICK ...").
 */
void sntp_time_format_countdown(int64_t target_epoch, const char *fmt, char *out, size_t out_cap);

/**
 * Return true if current local time falls within quiet hours [start_hhmm, end_hhmm).
 */
bool sntp_time_in_quiet_hours(bool enabled, const char *start_hhmm, const char *end_hhmm);

#ifdef __cplusplus
}
#endif
