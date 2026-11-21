/**
 * @file http_date_internal.h
 * @brief Locale-independent day and month names for HTTP and log dates.
 *
 * strftime("%a") and "%b" follow LC_TIME, so a program that calls
 * setlocale(LC_ALL, "") would emit localized names. HTTP dates
 * (RFC 9110 section 5.6.7) and Common Log Format need the English ones.
 */
#ifndef CWIST_HTTP_DATE_INTERNAL_H
#define CWIST_HTTP_DATE_INTERNAL_H

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *const cwist_http_day_names[7] = {"Sun", "Mon", "Tue", "Wed",
                                                    "Thu", "Fri", "Sat"};
static const char *const cwist_http_month_names[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

/**
 * @brief Format a broken-down UTC time as an IMF-fixdate.
 * @param tm Time in UTC.
 * @param buf Output buffer; 30 bytes hold the full date.
 * @param len Size of @p buf.
 */
static inline void cwist_http_format_tm(const struct tm *tm, char *buf, size_t len) {
    int wday = tm->tm_wday >= 0 && tm->tm_wday < 7 ? tm->tm_wday : 0;
    int mon = tm->tm_mon >= 0 && tm->tm_mon < 12 ? tm->tm_mon : 0;
    if (!buf || len == 0) return;
    /* Format into a buffer wide enough for any int fields, then copy, so a
     * short @p buf truncates instead of overflowing. */
    char tmp[96];
    int n = snprintf(tmp, sizeof(tmp), "%s, %02d %s %04d %02d:%02d:%02d GMT",
                     cwist_http_day_names[wday], tm->tm_mday, cwist_http_month_names[mon],
                     tm->tm_year + 1900, tm->tm_hour, tm->tm_min, tm->tm_sec);
    size_t used = n < 0 ? 0 : (size_t)n;
    if (used >= len) used = len - 1;
    memcpy(buf, tmp, used);
    buf[used] = '\0';
}

#endif
