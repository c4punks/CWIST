#ifndef CWIST_HTTP_COND_CLOCK_H
#define CWIST_HTTP_COND_CLOCK_H

#include <pthread.h>
#include <time.h>

static inline clockid_t cwist_cond_clockid(void) {
#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    return CLOCK_MONOTONIC;
#else
    return CLOCK_REALTIME;
#endif
}

static inline int cwist_cond_init_clock(pthread_cond_t *cond) {
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0) return -1;
#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    if (pthread_condattr_setclock(&attr, cwist_cond_clockid()) != 0) {
        pthread_condattr_destroy(&attr);
        return -1;
    }
#endif
    int rc = pthread_cond_init(cond, &attr);
    pthread_condattr_destroy(&attr);
    return rc;
}

#endif
