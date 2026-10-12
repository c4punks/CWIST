/**
 * @file h3_hdr_out.h
 * @brief Outgoing HTTP/3 header set builder for lsquic (private).
 */

#ifndef CWIST_NET_HTTP_H3_HDR_OUT_H
#define CWIST_NET_HTTP_H3_HDR_OUT_H

#include <lsquic.h>
#include <lsxpack_header.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Outgoing header set under construction for lsquic_stream_send_headers():
 * each field is copied into buf as the name, two spare bytes, then the value,
 * and the next lsxpack_header slot points at it. */
typedef struct {
    struct lsxpack_header *arr;
    size_t count, max;
    char *buf;
    size_t off, cap;
} h3_hdr_out;

/** Append one field. Returns false, adding nothing, when out of slots or room. */
static inline bool h3_hdr_out_add(h3_hdr_out *h, const char *name, size_t nlen, const char *val,
                                  size_t vlen) {
    if (h->count >= h->max || h->off + nlen + 2 + vlen > h->cap) return false;
    memcpy(h->buf + h->off, name, nlen);
    memcpy(h->buf + h->off + nlen + 2, val, vlen);
    lsxpack_header_set_offset2(&h->arr[h->count], h->buf + h->off, 0, nlen, nlen + 2, vlen);
    h->off += nlen + 2 + vlen;
    h->count++;
    return true;
}

static inline bool h3_hdr_out_add_str(h3_hdr_out *h, const char *name, const char *val) {
    return h3_hdr_out_add(h, name, strlen(name), val, strlen(val));
}

#endif /* CWIST_NET_HTTP_H3_HDR_OUT_H */
