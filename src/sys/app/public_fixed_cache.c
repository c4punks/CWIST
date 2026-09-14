#define _POSIX_C_SOURCE 200809L
#include "public_fixed_cache.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* These allocations MUST NOT participate in per-thread full GC. Both cache
 * entries and hit snapshots cross request/thread lifetime boundaries. Every
 * allocation has one explicit free, including all failure paths. */
typedef struct entry {
    struct entry *next;
    const void *route;
    cwist_http_method_t method;
    uint64_t created;
    size_t cost, path_len, host_len, body_len;
    char content_type[CWIST_PFC_TYPE_MAX + 1];
    unsigned char data[]; /* path, Host, binary body; length delimited */
} entry;
struct cwist_pfc {
    entry *entries;
    size_t count, bytes;
    uint64_t (*clock_seconds)(void);
    void *(*alloc)(size_t);
};
/* One lock covers every app's entries and the process-wide reservation.
 * Pressure evicts from the inserting cache, or safely misses if it is empty.
 * No foreign app pointer registry or lock ordering is needed. */
static pthread_mutex_t budget_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t process_count, process_bytes;
static bool text(const cwist_sstring *s) {
    return s && s->data && !memchr(s->data, 0, s->size);
}
static bool eq(const cwist_sstring *s, const char *v) {
    return text(s) && s->size == strlen(v) && !memcmp(s->data, v, s->size);
}
static bool ieq(const cwist_sstring *s, const char *v) {
    return text(s) && s->size == strlen(v) && !strncasecmp(s->data, v, s->size);
}
static bool field_value(const cwist_sstring *s) {
    if (!text(s)) return false;
    for (size_t i=0; i<s->size; ++i) {
        unsigned char c=(unsigned char)s->data[i];
        if ((c<32 && c!='\t') || c==127) return false;
    }
    return true;
}
static bool host_valid(const cwist_sstring *s) {
    if (!text(s) || !s->size || s->size>1024) return false;
    size_t port=s->size;
    if (s->data[0]=='[') {
        const char *end=memchr(s->data,']',s->size);
        if (!end) return false;
        size_t n=(size_t)(end-s->data)-1;
        char addr[INET6_ADDRSTRLEN]; struct in6_addr parsed;
        if (!n || n>=sizeof(addr)) return false;
        memcpy(addr,s->data+1,n); addr[n]=0;
        if (inet_pton(AF_INET6,addr,&parsed)!=1) return false;
        port=(size_t)(end-s->data)+1;
        if (port<s->size && s->data[port++]!=':') return false;
    } else {
        size_t n=0;
        for (; n<s->size && s->data[n]!=':'; ++n) {
            unsigned char c=(unsigned char)s->data[n];
            if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='-')) return false;
        }
        if (!n) return false;
        if (n<s->size) port=n+1;
    }
    if (s->data[s->size-1]==':') return false;
    unsigned int number=0;
    for (size_t i=port; i<s->size; ++i) {
        if (s->data[i]<'0'||s->data[i]>'9') return false;
        number=number*10+(unsigned int)(s->data[i]-'0');
        if (number>65535) return false;
    }
    return true;
}
static bool opts_ok(cwist_endpoint_opt_t opts) {
    return (opts & CWIST_ENDPOINT_PUBLIC_FIXED) &&
           !(opts & ~(CWIST_ENDPOINT_PUBLIC_FIXED|CWIST_ENDPOINT_FIXED));
}
bool cwist_pfc_request(const cwist_http_request *q, const void *route,
                       cwist_endpoint_opt_t opts, bool middleware, cwist_pfc_key *out) {
    if (!q || !route || !out || middleware || !opts_ok(opts) ||
        q->method!=CWIST_HTTP_GET || !eq(q->version,"HTTP/1.1") ||
        !text(q->path) || !q->path->size || q->path->size>8192 || q->path->data[0]!='/' ||
        memchr(q->path->data,'?',q->path->size) || memchr(q->path->data,'#',q->path->size) ||
        (q->query && q->query->size) || (q->body && q->body->size) ||
        q->content_length || q->te_chunked_seen || q->expect_100_seen || q->upgraded ||
        q->session || q->csrf_token || q->flash || q->private_data || q->path_params ||
        q->route_middleware_state || q->stream_id || q->https_conn || q->h2_queue) return false;
    unsigned int seen=0;
    const cwist_sstring *host=NULL;
    for (const cwist_http_header_node *h=q->headers; h; h=h->next) {
        unsigned int bit=0;
        if (!field_value(h->value)) return false;
        if (ieq(h->key,"Host")) { bit=1; host=h->value; if (!host_valid(host)) return false; }
        else if (ieq(h->key,"User-Agent")) bit=2;
        else if (ieq(h->key,"Accept")) { bit=4; if (!eq(h->value,"*/*")) return false; }
        else if (ieq(h->key,"Connection")) {
            bit=8;
            if (!ieq(h->value,"keep-alive") && !ieq(h->value,"close")) return false;
        } else return false;
        if (seen & bit) return false;
        seen |= bit;
    }
    if (!host) return false;
    *out=(cwist_pfc_key){.route=route,.method=q->method,.path=q->path->data,
        .path_len=q->path->size,.host=host->data,.host_len=host->size};
    return true;
}
bool cwist_pfc_response(const cwist_http_request *q, const cwist_http_response *r) {
    if (!q || !r || q->upgraded || q->session || q->csrf_token || q->flash || q->private_data ||
        !opts_ok(r->endpoint_opts) || r->status_code!=200 || !r->keep_alive ||
        !eq(r->version,"HTTP/1.1") || !eq(r->status_text,"OK") ||
        r->deferred || r->async || r->use_file_stream || r->alt_svc) return false;
    if (r->is_ptr_body) {
        if (r->ptr_body_len>CWIST_PFC_BODY_MAX || (r->ptr_body_len && !r->ptr_body)) return false;
    } else if (r->ptr_body || r->ptr_body_len || r->ptr_body_cleanup || r->ptr_body_cleanup_ctx ||
               !r->body || r->body->size>CWIST_PFC_BODY_MAX || (r->body->size && !r->body->data)) return false;
    if (r->headers && (r->headers->next || !ieq(r->headers->key,"Content-Type") ||
        !field_value(r->headers->value) || !r->headers->value->size ||
        r->headers->value->size>CWIST_PFC_TYPE_MAX)) return false;
    return true;
}
static uint64_t monotonic_seconds(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC,&t)!=0) return UINT64_MAX;
    return (uint64_t)t.tv_sec;
}
cwist_pfc *cwist_pfc_create_with(uint64_t (*clock_seconds)(void), void *(*alloc)(size_t)) {
    if (!clock_seconds || !alloc) return NULL;
    cwist_pfc *c=alloc(sizeof(*c));
    if (!c) return NULL;
    memset(c,0,sizeof(*c)); c->clock_seconds=clock_seconds; c->alloc=alloc;
    return c;
}
cwist_pfc *cwist_pfc_create(void) { return cwist_pfc_create_with(monotonic_seconds,malloc); }
static void remove_entry(cwist_pfc *c, entry **p) {
    entry *e=*p; *p=e->next; c->bytes-=e->cost; --c->count;
    process_bytes-=e->cost; --process_count; free(e);
}
void cwist_pfc_clear(cwist_pfc *c) {
    if (!c) return;
    pthread_mutex_lock(&budget_lock);
    while (c->entries) remove_entry(c,&c->entries);
    pthread_mutex_unlock(&budget_lock);
}
void cwist_pfc_destroy(cwist_pfc *c) {
    if (!c) return;
    cwist_pfc_clear(c); free(c);
}
static bool key_valid(const cwist_pfc_key *k) {
    return k && k->route && k->method==CWIST_HTTP_GET && k->path && k->host &&
        k->path_len && k->path_len<=8192 && k->host_len && k->host_len<=1024;
}
static bool matches(const entry *e, const cwist_pfc_key *k) {
    return e->route==k->route && e->method==k->method && e->path_len==k->path_len &&
        e->host_len==k->host_len && !memcmp(e->data,k->path,k->path_len) &&
        !memcmp(e->data+e->path_len,k->host,k->host_len);
}
static void expire(cwist_pfc *c,uint64_t now) {
    entry **p=&c->entries;
    while (*p) {
        if (now==UINT64_MAX || now<(*p)->created || now-(*p)->created>=CWIST_PFC_AGE_MAX) remove_entry(c,p);
        else p=&(*p)->next;
    }
}
bool cwist_pfc_get(cwist_pfc *c,const cwist_pfc_key *k,cwist_pfc_snapshot **out) {
    if (!c || !key_valid(k) || !out) return false;
    bool found=false;
    pthread_mutex_lock(&budget_lock); expire(c,c->clock_seconds());
    for (entry *e=c->entries;e;e=e->next) {
        if (!matches(e,k)) continue;
        cwist_pfc_snapshot *s=c->alloc(sizeof(*s)+e->body_len);
        if (s) {
            s->body_len=e->body_len; memcpy(s->content_type,e->content_type,sizeof(s->content_type));
            memcpy(s->body,e->data+e->path_len+e->host_len,e->body_len);
            *out=s; found=true;
        }
        break;
    }
    pthread_mutex_unlock(&budget_lock); return found;
}
bool cwist_pfc_put(cwist_pfc *c,const cwist_pfc_key *k,const cwist_http_request *q,const cwist_http_response *r) {
    if (!c || !key_valid(k) || !cwist_pfc_response(q,r)) return false;
    cwist_pfc_key actual;
    if (!cwist_pfc_request(q,k->route,r->endpoint_opts,false,&actual) ||
        actual.method!=k->method || actual.path_len!=k->path_len || actual.host_len!=k->host_len ||
        memcmp(actual.path,k->path,k->path_len) || memcmp(actual.host,k->host,k->host_len)) return false;
    size_t len=r->is_ptr_body?r->ptr_body_len:r->body->size;
    /* All summands bounded above before allocation; total below 76 KiB. */
    size_t cost=sizeof(entry)+k->path_len+k->host_len+len;
    pthread_mutex_lock(&budget_lock);
    uint64_t now=c->clock_seconds(); expire(c,now);
    if (now==UINT64_MAX) { pthread_mutex_unlock(&budget_lock); return false; }
    /* Evict before allocation: even a temporary replacement must fit. */
    while (process_count>=CWIST_PFC_ENTRIES_MAX || process_bytes>CWIST_PFC_BYTES_MAX-cost) {
        entry **victim=&c->entries;
        if (!*victim) { pthread_mutex_unlock(&budget_lock); return false; }
        while ((*victim)->next) victim=&(*victim)->next;
        remove_entry(c,victim);
    }
    entry *e=c->alloc(cost);
    if (!e) { pthread_mutex_unlock(&budget_lock); return false; }
    memset(e,0,sizeof(*e)); e->route=k->route; e->method=k->method; e->created=now;
    e->cost=cost; e->path_len=k->path_len; e->host_len=k->host_len; e->body_len=len;
    memcpy(e->data,k->path,k->path_len); memcpy(e->data+k->path_len,k->host,k->host_len);
    if (len) memcpy(e->data+k->path_len+k->host_len,r->is_ptr_body?r->ptr_body:r->body->data,len);
    if (r->headers) memcpy(e->content_type,r->headers->value->data,r->headers->value->size);
    entry **p=&c->entries;
    while (*p) { if (matches(*p,k)) remove_entry(c,p); else p=&(*p)->next; }
    e->next=c->entries; c->entries=e; ++c->count; c->bytes+=cost;
    ++process_count; process_bytes+=cost;
    pthread_mutex_unlock(&budget_lock); return true;
}
void cwist_pfc_snapshot_free(cwist_pfc_snapshot *s) { free(s); }
size_t cwist_pfc_count(cwist_pfc *c) {
    if (!c) return 0;
    pthread_mutex_lock(&budget_lock); size_t n=c->count; pthread_mutex_unlock(&budget_lock); return n;
}
size_t cwist_pfc_bytes(cwist_pfc *c) {
    if (!c) return 0;
    pthread_mutex_lock(&budget_lock); size_t n=c->bytes; pthread_mutex_unlock(&budget_lock); return n;
}
