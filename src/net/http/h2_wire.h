/**
 * @file h2_wire.h
 * @brief HTTP/2 wire integers in network byte order (private).
 *
 * RFC 9113 frames carry 24-bit lengths, 16-bit setting identifiers, 32-bit
 * error codes and setting values, and 31-bit stream identifiers and window
 * increments whose top bit is reserved (sent as 0, ignored on receipt).
 */

#ifndef CWIST_NET_HTTP_H2_WIRE_H
#define CWIST_NET_HTTP_H2_WIRE_H

#include <stdint.h>

static inline void h2_put_u24(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 16);
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)v;
}

static inline void h2_put_u32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

/** Stream identifier or window increment: the reserved bit goes out as 0. */
static inline void h2_put_u31(unsigned char *p, uint32_t v) {
    h2_put_u32(p, v & 0x7fffffffu);
}

static inline uint16_t h2_get_u16(const unsigned char *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline uint32_t h2_get_u24(const unsigned char *p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static inline uint32_t h2_get_u32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/** Stream identifier or window increment: the reserved bit is ignored. */
static inline uint32_t h2_get_u31(const unsigned char *p) {
    return h2_get_u32(p) & 0x7fffffffu;
}

#endif /* CWIST_NET_HTTP_H2_WIRE_H */
