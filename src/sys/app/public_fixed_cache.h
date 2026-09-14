/* Private representation cache. Configuration/destruction require quiescent
 * workers, like the router. Entries and snapshots use libc, never thread GC. */
#ifndef CWIST_PUBLIC_FIXED_CACHE_H
#define CWIST_PUBLIC_FIXED_CACHE_H
#include <cwist/net/http/http.h>
#define CWIST_PFC_BODY_MAX (64u * 1024u)
#define CWIST_PFC_TYPE_MAX 1024u
#define CWIST_PFC_ENTRIES_MAX 256u
#define CWIST_PFC_BYTES_MAX (16u * 1024u * 1024u)
#define CWIST_PFC_AGE_MAX 60u
typedef struct cwist_pfc cwist_pfc;
typedef struct {
    const void *route;
    const char *path, *host;
    size_t path_len, host_len;
    cwist_http_method_t method;
} cwist_pfc_key;
typedef struct {
    size_t body_len;
    char content_type[CWIST_PFC_TYPE_MAX + 1];
    unsigned char body[];
} cwist_pfc_snapshot;
bool cwist_pfc_request(const cwist_http_request *, const void *route,
                       cwist_endpoint_opt_t, bool middleware, cwist_pfc_key *);
bool cwist_pfc_response(const cwist_http_request *, const cwist_http_response *);
cwist_pfc *cwist_pfc_create(void);
/* Private deterministic clock/allocation seam. Allocator must be free-compatible. */
cwist_pfc *cwist_pfc_create_with(uint64_t (*clock_seconds)(void), void *(*alloc)(size_t));
void cwist_pfc_clear(cwist_pfc *);
void cwist_pfc_destroy(cwist_pfc *);
/* On hit, returns a caller-owned copy. A miss leaves *out unchanged. */
bool cwist_pfc_get(cwist_pfc *, const cwist_pfc_key *, cwist_pfc_snapshot **);
bool cwist_pfc_put(cwist_pfc *, const cwist_pfc_key *, const cwist_http_request *, const cwist_http_response *);
void cwist_pfc_snapshot_free(cwist_pfc_snapshot *);
size_t cwist_pfc_count(cwist_pfc *);
size_t cwist_pfc_bytes(cwist_pfc *);
#endif
