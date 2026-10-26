/**
 * @file baremetal_time.c
 * @brief clock_gettime(), nanosleep() and timegm() for CWIST_BAREMETAL.
 *
 * newlib declares these once -D_POSIX_TIMERS / -D_POSIX_MONOTONIC_CLOCK are
 * set (see BAREMETAL_CFLAGS) but implements none of them:
 *
 *   - clock_gettime(CLOCK_MONOTONIC) comes from cwist_port_monotonic_ns();
 *     CLOCK_REALTIME from gettimeofday(), i.e. the firmware's RTC if any.
 *   - nanosleep() busy-waits on the monotonic clock.
 *   - timegm() is Howard Hinnant's days_from_civil.
 *
 * Kept apart from baremetal.c so tests/test_baremetal_port.c can build it on
 * the host with the three names renamed (-Dclock_gettime=... etc.).
 */

#include <cwist/sys/platform.h>

#if defined(CWIST_BAREMETAL)

#include <cwist/sys/port.h>

#include <errno.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>

int clock_gettime(clockid_t id, struct timespec *ts) {
    if (id == CLOCK_REALTIME) {
        struct timeval tv;
        if (gettimeofday(&tv, NULL) != 0) return -1;
        ts->tv_sec = tv.tv_sec;
        ts->tv_nsec = (long)tv.tv_usec * 1000L;
        return 0;
    }
    if (id != CLOCK_MONOTONIC) {
        errno = EINVAL;
        return -1;
    }
    uint64_t ns = cwist_port_monotonic_ns();
    ts->tv_sec = (time_t)(ns / 1000000000u);
    ts->tv_nsec = (long)(ns % 1000000000u);
    return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    if (!req || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    uint64_t until = cwist_port_monotonic_ns() + (uint64_t)req->tv_sec * 1000000000u +
                     (uint64_t)req->tv_nsec;
    while (cwist_port_monotonic_ns() < until) {
    }
    if (rem) rem->tv_sec = 0, rem->tv_nsec = 0;
    return 0;
}

/* Days from 1970-01-01 to y-m-d (proleptic Gregorian), Howard Hinnant's
 * days_from_civil. */
time_t timegm(struct tm *tm) {
    long long y = (long long)tm->tm_year + 1900;
    int m = tm->tm_mon + 1;
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (unsigned)(m > 2 ? m - 3 : m + 9) + 2u) / 5u + (unsigned)tm->tm_mday - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    long long days = era * 146097LL + (long long)doe - 719468LL;
    return (time_t)(days * 86400LL + tm->tm_hour * 3600LL + tm->tm_min * 60LL + tm->tm_sec);
}

#endif /* CWIST_BAREMETAL */
