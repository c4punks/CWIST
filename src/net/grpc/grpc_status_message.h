#ifndef CWIST_GRPC_STATUS_MESSAGE_H
#define CWIST_GRPC_STATUS_MESSAGE_H

#include <cwist/core/mem/alloc.h>
#include <ctype.h>
#include <stdint.h>
#include <string.h>

static char *cwist_grpc_status_message_encode(const char *message) {
    if (!message) return NULL;
    static const char hex[] = "0123456789ABCDEF";
    size_t len = strlen(message);
    if (len > (SIZE_MAX - 1) / 3) return NULL;
    char *encoded = cwist_alloc(len * 3 + 1);
    if (!encoded) return NULL;
    size_t out = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)message[i];
        if (c >= 0x20 && c <= 0x7e && c != '%') {
            encoded[out++] = (char)c;
        } else {
            encoded[out++] = '%';
            encoded[out++] = hex[c >> 4];
            encoded[out++] = hex[c & 0x0f];
        }
    }
    encoded[out] = '\0';
    return encoded;
}

static char *cwist_grpc_status_message_decode(const char *message) {
    if (!message) return NULL;
    size_t len = strlen(message);
    char *decoded = cwist_alloc(len + 1);
    if (!decoded) return NULL;
    size_t out = 0;
    for (size_t i = 0; i < len; i++) {
        if (message[i] == '%' && i + 2 < len && isxdigit((unsigned char)message[i + 1]) &&
            isxdigit((unsigned char)message[i + 2])) {
            unsigned int high = (unsigned char)message[i + 1];
            unsigned int low = (unsigned char)message[i + 2];
            high = high <= '9' ? high - '0' : (high | 0x20) - 'a' + 10;
            low = low <= '9' ? low - '0' : (low | 0x20) - 'a' + 10;
            unsigned int value = (high << 4) | low;
            if (value != 0) {
                decoded[out++] = (char)value;
                i += 2;
                continue;
            }
        }
        decoded[out++] = message[i];
    }
    decoded[out] = '\0';
    return decoded;
}

#endif
