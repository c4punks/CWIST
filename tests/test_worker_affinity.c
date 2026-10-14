#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#ifdef __linux__
#include <errno.h>
#include <sched.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define REQUIRE(c)                                                                   \
    do {                                                                             \
        if (!(c)) {                                                                  \
            fprintf(stderr, "affinity check failed at line %d: %s\n", __LINE__, #c); \
            exit(1);                                                                 \
        }                                                                            \
    } while (0)

static bool mock_mode = true, fail_read, fail_write;
static cpu_set_t current;
static int writes;
static int read_mask(pid_t pid, size_t size, cpu_set_t *mask) {
    if (!mock_mode) return sched_getaffinity(pid, size, mask);
    REQUIRE(pid == 0 && size == sizeof(*mask));
    if (fail_read) {
        errno = EIO;
        return -1;
    }
    *mask = current;
    return 0;
}
static int write_mask(pid_t pid, size_t size, const cpu_set_t *mask) {
    if (!mock_mode) return sched_setaffinity(pid, size, mask);
    REQUIRE(pid == 0 && size == sizeof(*mask));
    writes++;
    if (fail_write) {
        errno = EPERM;
        return -1;
    }
    current = *mask;
    return 0;
}
/* Fake sysfs root; a missing directory means "no topology exposed". */
static char sysfs_root[256] = "/nonexistent-cwist-sysfs";
#define CWIST_APP_CPU_SYSFS sysfs_root
#define sched_getaffinity read_mask
#define sched_setaffinity write_mask
#include "worker_affinity.h"
#undef sched_getaffinity
#undef sched_setaffinity

static void check_pair(int first, int second, size_t index, int expected) {
    CPU_ZERO(&current);
    CPU_SET(first, &current);
    CPU_SET(second, &current);
    writes = 0;
    REQUIRE(cwist_app_pin_worker(index) == 0);
    REQUIRE(writes == 1);
    REQUIRE(CPU_COUNT(&current) == 1 && CPU_ISSET(expected, &current));
}
/* Build a fake topology: siblings[cpu] is the cpulist for that CPU. */
static void fake_topology(const char *dir, int ncpu, const char *const *siblings,
                          const char *file) {
    snprintf(sysfs_root, sizeof(sysfs_root), "%s", dir);
    char path[512];
    REQUIRE(mkdir(dir, 0700) == 0 || errno == EEXIST);
    for (int cpu = 0; cpu < ncpu; cpu++) {
        snprintf(path, sizeof(path), "%s/cpu%d", dir, cpu);
        REQUIRE(mkdir(path, 0700) == 0 || errno == EEXIST);
        snprintf(path, sizeof(path), "%s/cpu%d/topology", dir, cpu);
        REQUIRE(mkdir(path, 0700) == 0 || errno == EEXIST);
        snprintf(path, sizeof(path), "%s/cpu%d/topology/%s", dir, cpu, file);
        FILE *f = fopen(path, "w");
        REQUIRE(f);
        fprintf(f, "%s\n", siblings[cpu]);
        fclose(f);
    }
}
static void pin_order(int ncpu, const int *expected) {
    for (int i = 0; i < ncpu; i++) {
        CPU_ZERO(&current);
        for (int cpu = 0; cpu < ncpu; cpu++) CPU_SET(cpu, &current);
        writes = 0;
        REQUIRE(cwist_app_pin_worker((size_t)i) == 0);
        REQUIRE(writes == 1);
        REQUIRE(CPU_COUNT(&current) == 1 && CPU_ISSET(expected[i], &current));
    }
}
static void check_smt(void) {
    char base[] = "/tmp/cwist-affinity-XXXXXX";
    REQUIRE(mkdtemp(base));
    char dir[300];
    /* Adjacent siblings (0,1 2,3 ...): one thread per core first. */
    static const char *const adjacent[] = {"0-1", "0-1", "2-3", "2-3", "4-5", "4-5", "6-7", "6-7"};
    static const int adjacent_order[] = {0, 2, 4, 6, 1, 3, 5, 7};
    snprintf(dir, sizeof(dir), "%s/adjacent", base);
    fake_topology(dir, 8, adjacent, "core_cpus_list");
    pin_order(8, adjacent_order);
    /* Split siblings (0,4 1,5 ...): already one per core, order unchanged. */
    static const char *const split[] = {"0,4", "1,5", "2,6", "3,7", "0,4", "1,5", "2,6", "3,7"};
    static const int split_order[] = {0, 1, 2, 3, 4, 5, 6, 7};
    snprintf(dir, sizeof(dir), "%s/split", base);
    fake_topology(dir, 8, split, "core_cpus_list");
    pin_order(8, split_order);
    /* Older kernels: thread_siblings_list only, 4-way SMT. */
    static const char *const smt4[] = {"0-3", "0-3", "0-3", "0-3", "4-7", "4-7", "4-7", "4-7"};
    static const int smt4_order[] = {0, 4, 1, 5, 2, 6, 3, 7};
    snprintf(dir, sizeof(dir), "%s/smt4", base);
    fake_topology(dir, 8, smt4, "thread_siblings_list");
    pin_order(8, smt4_order);
    /* A mask holding one thread of core 0 and both of core 1: the lone
     * thread of core 0 counts as rank 0. */
    snprintf(sysfs_root, sizeof(sysfs_root), "%s/adjacent", base);
    CPU_ZERO(&current);
    CPU_SET(1, &current);
    CPU_SET(2, &current);
    CPU_SET(3, &current);
    REQUIRE(cwist_app_pin_worker(1) == 0 && CPU_ISSET(2, &current) && CPU_COUNT(&current) == 1);
    CPU_ZERO(&current);
    CPU_SET(1, &current);
    CPU_SET(2, &current);
    CPU_SET(3, &current);
    REQUIRE(cwist_app_pin_worker(2) == 0 && CPU_ISSET(3, &current) && CPU_COUNT(&current) == 1);
    /* Malformed file: falls back to plain ascending order. */
    static const char *const junk[] = {"x", "0-", "", "9999999"};
    static const int junk_order[] = {0, 1, 2, 3};
    snprintf(dir, sizeof(dir), "%s/junk", base);
    fake_topology(dir, 4, junk, "core_cpus_list");
    pin_order(4, junk_order);
    snprintf(sysfs_root, sizeof(sysfs_root), "/nonexistent-cwist-sysfs");
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", base);
    REQUIRE(system(cmd) == 0);
}
static void check_errors(void) {
    CPU_ZERO(&current);
    CPU_SET(3, &current);
    cpu_set_t before = current;
    writes = 0;
    fail_read = true;
    REQUIRE(cwist_app_pin_worker(0) == -1 && errno == EIO);
    REQUIRE(writes == 0 && CPU_EQUAL(&before, &current));
    fail_read = false;
    fail_write = true;
    REQUIRE(cwist_app_pin_worker(0) == -1 && errno == EPERM);
    REQUIRE(writes == 1 && CPU_EQUAL(&before, &current));
    fail_write = false;
    writes = 0;
    CPU_ZERO(&current);
    REQUIRE(cwist_app_pin_worker(0) == -1 && errno == EINVAL);
    REQUIRE(writes == 0 && CPU_COUNT(&current) == 0);
}
static void check_real_child(void) {
    cpu_set_t before, after;
    REQUIRE(sched_getaffinity(0, sizeof(before), &before) == 0);
    int last = -1;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++)
        if (CPU_ISSET(cpu, &before)) last = cpu;
    REQUIRE(last >= 0);
    pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        mock_mode = false;
        cpu_set_t one;
        CPU_ZERO(&one);
        CPU_SET(last, &one);
        REQUIRE(sched_setaffinity(0, sizeof(one), &one) == 0);
        REQUIRE(cwist_app_pin_worker(7) == 0);
        REQUIRE(sched_getaffinity(0, sizeof(after), &after) == 0);
        REQUIRE(CPU_EQUAL(&one, &after));
        _exit(0);
    }
    int status;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    REQUIRE(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    REQUIRE(sched_getaffinity(0, sizeof(after), &after) == 0);
    REQUIRE(CPU_EQUAL(&before, &after));
}
int main(void) {
    check_pair(1, 3, 0, 1);
    check_pair(1, 3, 1, 3);
    check_pair(1, 3, 7, 3);
    check_pair(0, 2, 1, 2);
    check_pair(0, 1, 0, 0);
    check_pair(0, 1, 1, 1);
    check_pair(CPU_SETSIZE - 1, CPU_SETSIZE - 1, 999, CPU_SETSIZE - 1);
    check_smt();
    check_errors();
    check_real_child();
    puts("Worker affinity tests passed (7 placements, SMT orders, 3 failures, real child).");
    return 0;
}
#else
int main(void) {
    puts("Worker affinity tests skipped: Linux only.");
    return 0;
}
#endif
