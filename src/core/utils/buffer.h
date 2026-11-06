#ifndef CWIST_CORE_UTILS_BUFFER_H
#define CWIST_CORE_UTILS_BUFFER_H

#include <cwist/core/mem/alloc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct cwist_buffer {
    char *data;
    size_t len;
    size_t capacity;
} cwist_buffer;

static inline void cwist_buffer_init(cwist_buffer *b) {
    b->data = NULL;
    b->len = 0;
    b->capacity = 0;
}

static inline void cwist_buffer_destroy(cwist_buffer *b) {
    cwist_free(b->data);
    cwist_buffer_init(b);
}

static inline bool cwist_buffer_reserve(cwist_buffer *b, size_t extra) {
    if (extra > SIZE_MAX - b->len) return false;
    size_t needed = b->len + extra;
    if (needed <= b->capacity) return true;

    size_t capacity = b->capacity ? b->capacity : 4096;
    while (capacity < needed) {
        if (capacity < 65536) {
            capacity = capacity > 32768 ? 65536 : capacity * 2;
        } else {
            if (capacity > SIZE_MAX - 65536) return false;
            capacity += 65536;
        }
    }
    char *data = b->data ? (char *)cwist_realloc(b->data, capacity) : (char *)cwist_alloc(capacity);
    if (!data) return false;
    b->data = data;
    b->capacity = capacity;
    return true;
}

static inline bool cwist_buffer_append(cwist_buffer *b, const void *data, size_t len) {
    if (!cwist_buffer_reserve(b, len)) return false;
    if (len) memcpy(b->data + b->len, data, len);
    b->len += len;
    return true;
}

#endif
