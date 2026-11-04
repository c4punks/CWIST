#ifndef CWIST_WS_FRAME_VALIDATION_H
#define CWIST_WS_FRAME_VALIDATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline bool cwist_ws_internal_send_args_valid(uint8_t opcode, size_t len) {
    if (opcode != 0x0 && opcode != 0x1 && opcode != 0x2 && opcode != 0x8 && opcode != 0x9 &&
        opcode != 0xA)
        return false;
    return opcode < 0x8 || len <= 125;
}

static inline bool cwist_ws_internal_close_code_valid(uint16_t code) {
    if (code < 1000 || code >= 5000 || code == 1004 || code == 1005 || code == 1006 ||
        code == 1015)
        return false;
    return code <= 1014 || code >= 3000;
}

static inline bool cwist_ws_internal_utf8_valid(const uint8_t *data, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t c = data[i++];
        if (c < 0x80) continue;
        if (c >= 0xC2 && c <= 0xDF) {
            if (i >= len || (data[i] & 0xC0) != 0x80) return false;
            i++;
            continue;
        }
        if (c >= 0xE0 && c <= 0xEF) {
            if (i + 1 >= len || (data[i + 1] & 0xC0) != 0x80) return false;
            if (c == 0xE0) {
                if (data[i] < 0xA0 || data[i] > 0xBF) return false;
            } else if (c == 0xED) {
                if (data[i] < 0x80 || data[i] > 0x9F) return false;
            } else if ((data[i] & 0xC0) != 0x80) {
                return false;
            }
            i += 2;
            continue;
        }
        if (c >= 0xF0 && c <= 0xF4) {
            if (i + 2 >= len || (data[i + 1] & 0xC0) != 0x80 ||
                (data[i + 2] & 0xC0) != 0x80)
                return false;
            if (c == 0xF0) {
                if (data[i] < 0x90 || data[i] > 0xBF) return false;
            } else if (c == 0xF4) {
                if (data[i] < 0x80 || data[i] > 0x8F) return false;
            } else if ((data[i] & 0xC0) != 0x80) {
                return false;
            }
            i += 3;
            continue;
        }
        return false;
    }
    return true;
}

static inline int cwist_ws_internal_close_error(const uint8_t *payload, size_t len) {
    if (len == 0) return 0;
    if (len == 1) return 1002;
    uint16_t code = (uint16_t)((uint16_t)payload[0] << 8 | payload[1]);
    if (!cwist_ws_internal_close_code_valid(code)) return 1002;
    return cwist_ws_internal_utf8_valid(payload + 2, len - 2) ? 0 : 1007;
}

#endif
