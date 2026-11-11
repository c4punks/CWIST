/**
 * @file tls_info.h
 * @brief Negotiated TLS parameters of the connection a request arrived on.
 *
 * Internal. Implemented in https.c.
 */

#ifndef CWIST_NET_HTTP_TLS_INFO_H
#define CWIST_NET_HTTP_TLS_INFO_H

#include <cwist/net/http/http.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct cwist_tls_info {
    uint16_t version; /**< TLS1_2_VERSION, TLS1_3_VERSION, ... */
    uint16_t group;   /**< IANA key-exchange group id; 0 when unknown */
} cwist_tls_info;

/**
 * Fill @p out for @p req when it arrived over TLS: TCP TLS (HTTP/1.1 and h2)
 * from the connection's SSL, HTTP/3 from the engine's handshake record while
 * its handler runs. Returns false for cleartext requests or outside a handler.
 */
bool cwist_http_request_tls_info(const cwist_http_request *req, cwist_tls_info *out);

/** True for hybrid post-quantum key-exchange groups (e.g. X25519MLKEM768). */
bool cwist_tls_group_is_pq(uint16_t group);

/** HTTP/3 engine thread: publish the group of @p req's connection during its handler. */
void cwist_tls_info_h3_set(const cwist_http_request *req, uint16_t group);
void cwist_tls_info_h3_clear(void);

#endif
