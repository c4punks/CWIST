#ifndef CWIST_APP_WORKER_AFFINITY_H
#define CWIST_APP_WORKER_AFFINITY_H

/* Private Linux startup helper; callers must enable GNU affinity APIs. */
#include <errno.h>
#include <sched.h>
#include <stddef.h>

static inline int cwist_app_pin_worker(size_t worker_index) {
    cpu_set_t allowed;
    if (sched_getaffinity(0, sizeof(allowed), &allowed) < 0) return -1;
    int count = CPU_COUNT(&allowed);
    if (count == 0) {
        errno = EINVAL;
        return -1;
    }
    size_t ordinal = worker_index % (size_t)count;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (!CPU_ISSET(cpu, &allowed)) continue;
        if (ordinal != 0) {
            ordinal--;
            continue;
        }
        cpu_set_t selected;
        CPU_ZERO(&selected);
        CPU_SET(cpu, &selected);
        return sched_setaffinity(0, sizeof(selected), &selected);
    }
    errno = EINVAL;
    return -1;
}

#endif
