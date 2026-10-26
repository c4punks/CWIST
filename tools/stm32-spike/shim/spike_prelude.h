/* spike: newlib hides CLOCK_MONOTONIC without _POSIX_MONOTONIC_CLOCK */
#include <time.h>
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC ((clockid_t)4)
#endif
#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW ((clockid_t)4)
#endif
int clock_gettime(clockid_t, struct timespec *);
int sched_yield(void);
time_t timegm(struct tm *);
int nanosleep(const struct timespec *, struct timespec *);
