#ifndef CWIST_HTTP3_GLOBAL_H
#define CWIST_HTTP3_GLOBAL_H

void cwist_h3_global_acquire(void);
void cwist_h3_global_release(void);

struct cwist_http3_context;
/**
 * Apply a key-exchange group list (BoringSSL syntax) to the context's
 * SSL_CTX and keep it for cwist_http3_reload_context(). Call before the
 * server loop accepts connections. Returns 0, or -1 when rejected.
 */
int cwist_http3_internal_set_tls_groups(struct cwist_http3_context *ctx, const char *groups);

#endif
