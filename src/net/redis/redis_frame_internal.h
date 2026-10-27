/* Private RESP encoder shared by the client and focused bounds tests. */
#ifndef CWIST_REDIS_FRAME_INTERNAL_H
#define CWIST_REDIS_FRAME_INTERNAL_H
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static inline int redis_frame_fill(char *frame, size_t cap, size_t argc, const void *const *argv,
                                   const size_t *lens) {
    if (!frame || cap == 0 || argc == 0 || !argv || !lens) return -EINVAL;
    int n = snprintf(frame, cap, "*%zu\r\n", argc);
    if (n < 0 || (size_t)n >= cap) return -EINVAL;
    size_t used = (size_t)n;
    for (size_t i = 0; i < argc; ++i) {
        size_t len = lens[i];
        size_t remaining = cap - used;
        n = snprintf(frame + used, remaining, "$%zu\r\n", len);
        if (n < 0 || (size_t)n >= remaining) return -EINVAL;
        used += (size_t)n;
        remaining = cap - used;
        if (remaining < 3 || len > remaining - 3 || (len && !argv[i])) return -EINVAL;
        if (len) memcpy(frame + used, argv[i], len);
        used += len;
        frame[used++] = '\r';
        frame[used++] = '\n';
    }
    if (used != cap - 1) return -EINVAL;
    frame[used] = '\0';
    return 0;
}
#endif
