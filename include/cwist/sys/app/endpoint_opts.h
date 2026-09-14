/**
 * @file endpoint_opts.h
 * @brief Endpoint-level behavior flags for CWIST apps.
 */

#ifndef __CWIST_ENDPOINT_OPTS_H__
#define __CWIST_ENDPOINT_OPTS_H__

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Bitmask type controlling endpoint behavior.
 *
 * Flags can be OR-ed together to hint caching or streaming
 * strategies for a given route.
 */
typedef uint32_t cwist_endpoint_opt_t;

/** Immediate execution with no special tuning (default). */
#define CWIST_DYNAMIC         (1u << 0)
/**
 * Hint that the route's response is request-invariant.
 *
 * This compatibility hint alone does NOT activate automatic HTTP caching.
 * Every request uses ordinary dispatch in classic and C1M. Only the separate
 * CWIST_ENDPOINT_PUBLIC_FIXED assertion admits the restricted representation
 * cache (see docs/fixed-cache-status.md).
 */
#define CWIST_ENDPOINT_FIXED  (1u << 1)
/** Explicit public invariant representation assertion; independent of FIXED.
 * No auth/personalization or required per-request side effects. See
 * docs/fixed-cache-status.md for the restricted HTTP/1.1 admission profile. */
#define CWIST_ENDPOINT_PUBLIC_FIXED (1u << 3)
/** Serve large files using OS-specific zero-copy fast paths. */
#define CWIST_ENDPOINT_FILE   (1u << 2)

/** @brief Default option for new endpoints. */
#define CWIST_ENDPOINT_DEFAULT (CWIST_DYNAMIC)

static inline bool cwist_endpoint_has(cwist_endpoint_opt_t opts, cwist_endpoint_opt_t flag) {
    return (opts & flag) != 0;
}

#endif /* __CWIST_ENDPOINT_OPTS_H__ */
