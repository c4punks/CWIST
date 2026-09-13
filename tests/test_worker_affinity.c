#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#ifdef __linux__
#include <errno.h>
#include <sched.h>
#include <stdbool.h>
#include <sys/wait.h>
#include <unistd.h>

#define REQUIRE(c) do { if (!(c)) { \
    fprintf(stderr, "affinity check failed at line %d: %s\n", __LINE__, #c); \
    exit(1); \
} } while (0)

static bool mock_mode = true, fail_read, fail_write;
static cpu_set_t current;
static int writes;
static int read_mask(pid_t pid, size_t size, cpu_set_t *mask) {
    if (!mock_mode) return sched_getaffinity(pid, size, mask);
    REQUIRE(pid == 0 && size == sizeof(*mask));
    if (fail_read) { errno = EIO; return -1; }
    *mask = current;
    return 0;
}
static int write_mask(pid_t pid, size_t size, const cpu_set_t *mask) {
    if (!mock_mode) return sched_setaffinity(pid, size, mask);
    REQUIRE(pid == 0 && size == sizeof(*mask));
    writes++;
    if (fail_write) { errno = EPERM; return -1; }
    current = *mask;
    return 0;
}
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
static void check_errors(void) {
    CPU_ZERO(&current); CPU_SET(3, &current);
    cpu_set_t before = current;
    writes = 0; fail_read = true;
    REQUIRE(cwist_app_pin_worker(0) == -1 && errno == EIO);
    REQUIRE(writes == 0 && CPU_EQUAL(&before, &current));
    fail_read = false; fail_write = true;
    REQUIRE(cwist_app_pin_worker(0) == -1 && errno == EPERM);
    REQUIRE(writes == 1 && CPU_EQUAL(&before, &current));
    fail_write = false; writes = 0; CPU_ZERO(&current);
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
    pid_t child = fork(); REQUIRE(child >= 0);
    if (child == 0) {
        mock_mode = false;
        cpu_set_t one; CPU_ZERO(&one); CPU_SET(last, &one);
        REQUIRE(sched_setaffinity(0, sizeof(one), &one) == 0);
        REQUIRE(cwist_app_pin_worker(7) == 0);
        REQUIRE(sched_getaffinity(0, sizeof(after), &after) == 0);
        REQUIRE(CPU_EQUAL(&one, &after));
        _exit(0);
    }
    int status; pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
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
    check_errors();
    check_real_child();
    puts("Worker affinity tests passed (7 placements, 3 failures, real child).");
    return 0;
}
#else
int main(void) { puts("Worker affinity tests skipped: Linux only."); return 0; }
#endif
