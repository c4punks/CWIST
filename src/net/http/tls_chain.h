#ifndef CWIST_NET_HTTP_TLS_CHAIN_H
#define CWIST_NET_HTTP_TLS_CHAIN_H

#include <openssl/ssl.h>

int cwist_tls_autoload_intermediates(SSL_CTX *ssl_ctx);

/* Bounded retention for the BoringSSL server-side session cache.
 * CWIST_TLS_SESS_CACHE_SIZE cached getter (implemented in tls_chain.c):
 * the cache cap applied to every server SSL_CTX; 0 keeps the BoringSSL
 * default (no explicit cap). See docs/runtime-config.md. */
unsigned long cwist_tls_sess_cache_size(void);

#endif /* CWIST_NET_HTTP_TLS_CHAIN_H */
