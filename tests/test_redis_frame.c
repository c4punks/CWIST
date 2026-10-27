#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include "../src/net/redis/redis_frame_internal.h"

static char fault_kind;
static int fault_mode;
static int failed_format(char *buf, size_t cap, const char *fmt, ...) {
    if (fmt[0] == fault_kind) {
        if (fault_mode == 0) return -1;
        return fault_mode == 1 ? (int)cap : 0;
    }
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, cap, fmt, args);
    va_end(args);
    return n;
}

/* Compile the same encoder with a controlled formatter failure. */
#undef CWIST_REDIS_FRAME_INTERNAL_H
#define redis_frame_fill redis_frame_fill_fault
#undef snprintf
#define snprintf failed_format
#include "../src/net/redis/redis_frame_internal.h"
#undef snprintf
#undef redis_frame_fill

#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            fprintf(stderr, "frame check failed at %d\n", __LINE__); \
            return 1;                                                \
        }                                                            \
    } while (0)

int main(void) {
    const void *args[] = {"ECHO", "hello"};
    const size_t lens[] = {4, 5};
    const char expected[] = "*2\r\n$4\r\nECHO\r\n$5\r\nhello\r\n";
    unsigned char guarded[sizeof(expected) + 2];
    memset(guarded, 0xa5, sizeof(guarded));
    CHECK(redis_frame_fill((char *)guarded + 1, sizeof(expected), 2, args, lens) == 0);
    CHECK(memcmp(guarded + 1, expected, sizeof(expected)) == 0);
    const void *binary[] = {"ECHO", "a\0b"};
    const size_t binary_lens[] = {4, 3};
    const char binary_expected[] = "*2\r\n$4\r\nECHO\r\n$3\r\na\0b\r\n";
    CHECK(redis_frame_fill((char *)guarded + 1, sizeof(binary_expected), 2, binary, binary_lens) ==
          0);
    CHECK(memcmp(guarded + 1, binary_expected, sizeof(binary_expected)) == 0);
    for (size_t cap = 0; cap < sizeof(expected); ++cap) {
        memset(guarded, 0xa5, sizeof(guarded));
        CHECK(redis_frame_fill((char *)guarded + 1, cap, 2, args, lens) == -EINVAL);
        CHECK(guarded[0] == 0xa5);
        for (size_t i = cap + 1; i < sizeof(guarded); ++i) CHECK(guarded[i] == 0xa5);
    }
    for (int k = 0; k < 2; ++k) {
        fault_kind = k ? '$' : '*';
        for (fault_mode = 0; fault_mode < 3; ++fault_mode) {
            memset(guarded, 0xa5, sizeof(guarded));
            CHECK(redis_frame_fill_fault((char *)guarded + 1, sizeof(expected), 2, args, lens) ==
                  -EINVAL);
            CHECK(guarded[0] == 0xa5 && guarded[sizeof(guarded) - 1] == 0xa5);
        }
    }
    puts("Redis frame bounds and formatter failure checks passed");
    return 0;
}
