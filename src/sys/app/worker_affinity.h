#ifndef CWIST_APP_WORKER_AFFINITY_H
#define CWIST_APP_WORKER_AFFINITY_H

/* Private Linux startup helper; callers must enable GNU affinity APIs. */
#include <errno.h>
#include <sched.h>
#include <stddef.h>

#include <stdio.h>

#ifndef CWIST_APP_CPU_SYSFS
#define CWIST_APP_CPU_SYSFS "/sys/devices/system/cpu"
#endif

/* Parse a sysfs cpulist ("0,6" or "0-1,8-9") into @p out. Returns 0, or -1
 * on an unreadable or malformed file. */
static inline int cwist_app_read_cpulist(const char *path, cpu_set_t *out) {
    FILE *f = fopen(path, "re");
    if (!f) return -1;
    CPU_ZERO(out);
    int rc = -1, lo, hi;
    char sep;
    for (;;) {
        if (fscanf(f, "%d", &lo) != 1) break;
        hi = lo;
        int n = fscanf(f, "%c", &sep);
        if (n == 1 && sep == '-') {
            if (fscanf(f, "%d", &hi) != 1) break;
            n = fscanf(f, "%c", &sep);
        }
        if (lo < 0 || hi < lo || hi >= CPU_SETSIZE) break;
        for (int cpu = lo; cpu <= hi; cpu++) CPU_SET(cpu, out);
        if (n != 1 || sep == '\n') {
            rc = 0;
            break;
        }
        if (sep != ',') break;
    }
    fclose(f);
    return rc;
}

/* SMT siblings of @p cpu, @p cpu included. Falls back to {cpu} when the
 * topology is not exposed (containers without sysfs, odd kernels), which
 * keeps the plain ascending order. */
static inline void cwist_app_cpu_siblings(int cpu, cpu_set_t *out) {
    char path[512];
    snprintf(path, sizeof(path), "%s/cpu%d/topology/core_cpus_list", CWIST_APP_CPU_SYSFS, cpu);
    if (cwist_app_read_cpulist(path, out) == 0 && CPU_ISSET(cpu, out)) return;
    snprintf(path, sizeof(path), "%s/cpu%d/topology/thread_siblings_list", CWIST_APP_CPU_SYSFS,
             cpu);
    if (cwist_app_read_cpulist(path, out) == 0 && CPU_ISSET(cpu, out)) return;
    CPU_ZERO(out);
    CPU_SET(cpu, out);
}

/**
 * @brief Pin the calling thread to a single CPU chosen from its allowed set.
 *
 * Reads the calling thread's current CPU affinity mask and orders the allowed
 * CPUs so that each physical core comes up once before any core is used
 * twice: every allowed CPU gets a rank, the number of its allowed SMT
 * siblings with a lower ID, and CPUs are taken by (rank, ID). On a host that
 * numbers siblings next to each other (0,1 / 2,3) this places workers 0 and
 * 1 on CPUs 0 and 2 rather than on the two threads of one core. Without SMT,
 * or without readable topology, the order is plain ascending. The
 * (worker_index % number-of-allowed-CPUs)-th CPU in that order is selected
 * and the affinity mask restricted to it. Requires GNU affinity APIs
 * (_GNU_SOURCE) for sched_getaffinity/sched_setaffinity and the CPU_* macros.
 *
 * @param worker_index Ordinal of the worker; mapped onto the allowed CPU set
 *                     with modulo arithmetic so any value is accepted.
 * @return 0 on success, -1 on failure with errno set (EINVAL if the allowed
 *         set is empty or the selected CPU cannot be found/applied).
 */
static inline int cwist_app_pin_worker(size_t worker_index) {
    cpu_set_t allowed;
    if (sched_getaffinity(0, sizeof(allowed), &allowed) < 0) return -1;
    int count = CPU_COUNT(&allowed);
    if (count == 0) {
        errno = EINVAL;
        return -1;
    }
    size_t ordinal = worker_index % (size_t)count;
    /* rank[cpu]: allowed siblings below cpu. Only allowed CPUs are filled. */
    unsigned short rank[CPU_SETSIZE];
    int max_rank = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (!CPU_ISSET(cpu, &allowed)) continue;
        cpu_set_t sib;
        cwist_app_cpu_siblings(cpu, &sib);
        CPU_AND(&sib, &sib, &allowed);
        int r = 0;
        for (int other = 0; other < cpu; other++)
            if (CPU_ISSET(other, &sib)) r++;
        rank[cpu] = (unsigned short)r;
        if (r > max_rank) max_rank = r;
    }
    for (int r = 0; r <= max_rank; r++) {
        for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
            if (!CPU_ISSET(cpu, &allowed) || rank[cpu] != r) continue;
            if (ordinal != 0) {
                ordinal--;
                continue;
            }
            cpu_set_t selected;
            CPU_ZERO(&selected);
            CPU_SET(cpu, &selected);
            return sched_setaffinity(0, sizeof(selected), &selected);
        }
    }
    errno = EINVAL;
    return -1;
}

#endif
