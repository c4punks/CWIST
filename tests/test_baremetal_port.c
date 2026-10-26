/* Host-side tests for the bare-metal port layer (issue #447).
 *
 * Built without libcwist.a: siphash.c and src/sys/port/baremetal_time.c are
 * compiled with -DCWIST_BAREMETAL, and baremetal_time.c's libc names are
 * renamed (clock_gettime -> cwist_bm_clock_gettime, ...) so they do not
 * collide with the host libc. This test supplies the firmware hooks from
 * <cwist/sys/port.h> and checks how CWIST consumes them:
 *
 *   - the siphash seed is drawn from cwist_port_entropy() and the process
 *     aborts when the hook fails, rather than degrading to a guessable seed;
 *   - CLOCK_MONOTONIC and nanosleep() run on cwist_port_monotonic_ns();
 *   - timegm() matches known instants on both sides of the epoch.
 *
 * The cross-compiled firmware path is covered by `make baremetal-smoke`. */
#include <cwist/core/siphash/siphash.h>
#include <cwist/sys/port.h>

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int cwist_bm_clock_gettime(clockid_t id, struct timespec *ts);
int cwist_bm_nanosleep(const struct timespec *req, struct timespec *rem);
time_t cwist_bm_timegm(struct tm *tm);

/* --- firmware hooks ---------------------------------------------------- */

static unsigned g_entropy_calls;
static int g_entropy_fail;
static uint8_t g_entropy_byte;

int cwist_port_entropy(void *buf, size_t len) {
    g_entropy_calls++;
    if (g_entropy_fail) return -1;
    uint8_t *out = buf;
    for (size_t i = 0; i < len; i++) out[i] = g_entropy_byte++;
    return 0;
}

static uint64_t g_now_ns;
static uint64_t g_tick_ns; /* added on every read, like a free-running timer */

uint64_t cwist_port_monotonic_ns(void) {
    g_now_ns += g_tick_ns;
    return g_now_ns;
}

/* --- entropy ------------------------------------------------------------ */

static void test_seed_comes_from_hook(void) {
    uint8_t a[16], b[16];
    g_entropy_calls = 0;
    g_entropy_byte = 0;
    cwist_generate_hash_seed(a);
    assert(g_entropy_calls == 1);
    g_entropy_byte = 0;
    cwist_generate_hash_seed(b);
    assert(g_entropy_calls == 2);
    /* same hook bytes -> same key: nothing but the hook feeds the seed */
    assert(memcmp(a, b, sizeof(a)) == 0);
    g_entropy_byte = 0x80;
    cwist_generate_hash_seed(b);
    assert(memcmp(a, b, sizeof(a)) != 0);
}

static void test_seed_fails_closed(void) {
    fflush(NULL);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        g_entropy_fail = 1;
        uint8_t key[16];
        cwist_generate_hash_seed(key);
        _exit(0); /* reaching here means a seed was invented */
    }
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}

/* --- monotonic clock ------------------------------------------------------- */

static void test_monotonic_clock(void) {
    struct timespec ts;
    g_tick_ns = 0;
    g_now_ns = 5ull * 1000000000ull + 250;
    assert(cwist_bm_clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    assert(ts.tv_sec == 5 && ts.tv_nsec == 250);

    errno = 0;
    assert(cwist_bm_clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == -1 && errno == EINVAL);
}

static void test_nanosleep_waits_on_hook(void) {
    g_now_ns = 0;
    g_tick_ns = 1000; /* 1 us per read */
    struct timespec req = {.tv_sec = 0, .tv_nsec = 50000}, rem = {1, 1};
    assert(cwist_bm_nanosleep(&req, &rem) == 0);
    assert(g_now_ns >= 50000);
    assert(rem.tv_sec == 0 && rem.tv_nsec == 0);

    struct timespec bad = {.tv_sec = 0, .tv_nsec = 1000000000L};
    errno = 0;
    assert(cwist_bm_nanosleep(&bad, NULL) == -1 && errno == EINVAL);
    g_tick_ns = 0;
}

/* --- timegm --------------------------------------------------------------- */

static time_t utc(int y, int mon, int d, int h, int mi, int s) {
    struct tm tm = {.tm_year = y - 1900, .tm_mon = mon - 1, .tm_mday = d,
                    .tm_hour = h, .tm_min = mi, .tm_sec = s};
    return cwist_bm_timegm(&tm);
}

static void test_timegm(void) {
    assert(utc(1970, 1, 1, 0, 0, 0) == 0);
    assert(utc(2000, 2, 29, 12, 0, 0) == 951825600);
    assert(utc(2026, 10, 25, 0, 0, 0) == 1792886400);
    assert(utc(1969, 12, 31, 23, 59, 59) == -1);
    assert(utc(1900, 3, 1, 0, 0, 0) == -2203891200);
    /* agrees with the host libc across a range of days */
    for (int day = -800; day < 30000; day += 37) {
        time_t t = (time_t)day * 86400 + 3723;
        struct tm tm;
        assert(gmtime_r(&t, &tm));
        assert(cwist_bm_timegm(&tm) == t);
    }
}

int main(void) {
    test_seed_comes_from_hook();
    test_seed_fails_closed();
    test_monotonic_clock();
    test_nanosleep_waits_on_hook();
    test_timegm();
    printf("test_baremetal_port: all passed\n");
    return 0;
}
