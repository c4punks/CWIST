/**
 * @file proxy_internal.h
 * @brief Reverse proxy internals exposed to the in-tree unit tests.
 */

#ifndef CWIST_NET_PROXY_PROXY_INTERNAL_H
#define CWIST_NET_PROXY_PROXY_INTERNAL_H

#include <cwist/net/proxy/proxy.h>
#include <stdbool.h>

/** True when @p path is an origin-form path the proxy forwards unchanged. */
bool cwist_proxy_path_is_safe(const char *path);

/** True when @p query (raw, without '?') may be forwarded; NULL and "" are safe. */
bool cwist_proxy_query_is_safe(const char *query);

/** True when @p name is a non-empty RFC 9110 token. */
bool cwist_proxy_header_name_ok(const char *name);

/** True when @p value holds no CR, LF, NUL or other control byte except HTAB. */
bool cwist_proxy_header_value_ok(const char *value);

/**
 * Resolve the upstream URL for a request arriving on @p port with Host
 * @p host (may be NULL), raw @p path and raw @p query (may be NULL).
 * @param status_out When non-NULL and no URL is returned: 400 for an unsafe
 *        path or query, 421 when no origin on the port is the addressed one,
 *        404 when the origin matches but no route prefix does.
 * @return Heap URL (free with cwist_free), or NULL.
 */
char *cwist_proxy_resolve_url(unsigned short port, const char *host, const char *path,
                              const char *query, int *status_out);

#endif
