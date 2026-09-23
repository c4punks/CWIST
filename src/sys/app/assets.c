/**
 * @file assets.c
 * @brief Content-hashed in-memory assets: registration, URL lookup and serving.
 *
 * Assets are immutable once published. A registry is a newest-first singly
 * linked list whose head is swapped in atomically, and nodes are only freed
 * with the app, so a request that resolved an asset can keep using it while
 * another thread registers more.
 */

#include "assets_internal.h"
#include <cwist/sys/app/assets.h>
#include <cwist/core/crypto/sha256.h>
#include <cwist/core/mem/alloc.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define ASSET_HASH_HEX 16

struct cwist_asset {
    char *name;         ///< Logical name, e.g. "css/app.css"
    char *hashed_name;  ///< e.g. "css/app.0123456789abcdef.css"
    char *url;          ///< prefix + "/" + hashed_name
    char *content_type; ///< Content-Type header value
    char etag[ASSET_HASH_HEX + 3];
    unsigned char *data;
    size_t len;
    struct cwist_asset *next;
};

typedef struct cwist_asset_registry {
    char *prefix; ///< Normalized: "" for the root, otherwise "/x" with no trailing '/'
    _Atomic(struct cwist_asset *) head;
} cwist_asset_registry;

static cwist_error_t asset_result(int16_t code) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    err.error.err_i16 = code;
    return err;
}

static char *join3(const char *a, const char *b, const char *c) {
    size_t la = strlen(a), lb = strlen(b), lc = strlen(c);
    char *out = (char *)cwist_alloc(la + lb + lc + 1);
    if (!out) return NULL;
    memcpy(out, a, la);
    memcpy(out + la, b, lb);
    memcpy(out + la + lb, c, lc + 1);
    return out;
}

static char *normalize_prefix(const char *url_prefix) {
    if (!url_prefix || url_prefix[0] != '/') return NULL;
    size_t len = strlen(url_prefix);
    while (len > 0 && url_prefix[len - 1] == '/') len--;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)url_prefix[i];
        if (c <= 0x20 || c == 0x7f || c == '?' || c == '#' || c == '%') return NULL;
    }
    char *out = (char *)cwist_alloc(len + 1);
    if (!out) return NULL;
    memcpy(out, url_prefix, len);
    out[len] = '\0';
    return out;
}

static bool name_is_valid(const char *name) {
    if (!name || !*name || name[0] == '/') return false;
    const char *seg = name;
    for (const char *p = name;; p++) {
        char c = *p;
        if (c == '/' || c == '\0') {
            size_t seg_len = (size_t)(p - seg);
            if (seg_len == 0) return false;
            if (seg_len == 1 && seg[0] == '.') return false;
            if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') return false;
            if (c == '\0') return true;
            seg = p + 1;
            continue;
        }
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '-' || c == '_' || c == '~';
        if (!ok) return false;
    }
}

/** Offset of the extension dot in the last segment, or strlen(name) if none. */
static size_t extension_offset(const char *name) {
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    const char *dot = strrchr(base, '.');
    /* A leading dot names a dotfile, not an extension. */
    if (!dot || dot == base) return strlen(name);
    return (size_t)(dot - name);
}

static const char *guess_content_type(const char *name) {
    static const struct {
        const char *ext;
        const char *type;
    } table[] = {
        {".css", "text/css; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"},
        {".mjs", "text/javascript; charset=utf-8"},
        {".json", "application/json"},
        {".map", "application/json"},
        {".html", "text/html; charset=utf-8"},
        {".htm", "text/html; charset=utf-8"},
        {".txt", "text/plain; charset=utf-8"},
        {".svg", "image/svg+xml"},
        {".png", "image/png"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".gif", "image/gif"},
        {".webp", "image/webp"},
        {".ico", "image/x-icon"},
        {".woff", "font/woff"},
        {".woff2", "font/woff2"},
        {".wasm", "application/wasm"},
    };
    const char *ext = name + extension_offset(name);
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        const char *a = ext, *b = table[i].ext;
        while (*a && *b && (*a | 0x20) == *b) {
            a++;
            b++;
        }
        if (*a == '\0' && *b == '\0') return table[i].type;
    }
    return "application/octet-stream";
}

static void content_hash(const void *data, size_t len, char out[ASSET_HASH_HEX + 1]) {
    static const char hex[] = "0123456789abcdef";
    uint8_t digest[CWIST_SHA256_DIGEST_LEN];
    cwist_sha256_ctx ctx;
    cwist_sha256_init(&ctx);
    if (len > 0) cwist_sha256_update(&ctx, (const uint8_t *)data, len);
    cwist_sha256_final(&ctx, digest);
    for (size_t i = 0; i < ASSET_HASH_HEX / 2; i++) {
        out[2 * i] = hex[digest[i] >> 4];
        out[2 * i + 1] = hex[digest[i] & 0xf];
    }
    out[ASSET_HASH_HEX] = '\0';
}

static void asset_free(struct cwist_asset *asset) {
    if (!asset) return;
    cwist_free(asset->name);
    cwist_free(asset->hashed_name);
    cwist_free(asset->url);
    cwist_free(asset->content_type);
    cwist_free(asset->data);
    cwist_free(asset);
}

static cwist_asset_registry *registry_get(cwist_app *app, bool create) {
    if (app->assets || !create) return (cwist_asset_registry *)app->assets;
    cwist_asset_registry *reg = (cwist_asset_registry *)cwist_alloc(sizeof(*reg));
    if (!reg) return NULL;
    reg->prefix = cwist_strdup(CWIST_ASSET_DEFAULT_PREFIX);
    if (!reg->prefix) {
        cwist_free(reg);
        return NULL;
    }
    atomic_init(&reg->head, NULL);
    app->assets = reg;
    return reg;
}

cwist_error_t cwist_app_asset_prefix(cwist_app *app, const char *url_prefix) {
    if (!app) return asset_result(-1);
    char *prefix = normalize_prefix(url_prefix);
    if (!prefix) return asset_result(-1);
    cwist_asset_registry *reg = registry_get(app, true);
    if (!reg || atomic_load_explicit(&reg->head, memory_order_acquire)) {
        cwist_free(prefix);
        return asset_result(-1);
    }
    cwist_free(reg->prefix);
    reg->prefix = prefix;
    return asset_result(0);
}

/** Build a published asset; takes ownership of `data` (freed on failure). */
static struct cwist_asset *asset_build(const cwist_asset_registry *reg, const char *name,
                                       unsigned char *data, size_t len, const char *content_type,
                                       const char *hash) {
    struct cwist_asset *asset = (struct cwist_asset *)cwist_alloc(sizeof(*asset));
    if (!asset) {
        cwist_free(data);
        return NULL;
    }
    asset->data = data;
    asset->len = len;

    size_t ext = extension_offset(name);
    char *stem = (char *)cwist_alloc(ext + 1);
    if (stem) {
        memcpy(stem, name, ext);
        stem[ext] = '\0';
    }
    char *stem_hash = stem ? join3(stem, ".", hash) : NULL;
    cwist_free(stem);

    asset->name = cwist_strdup(name);
    asset->hashed_name = stem_hash ? join3(stem_hash, name + ext, "") : NULL;
    cwist_free(stem_hash);
    asset->url = asset->hashed_name ? join3(reg->prefix, "/", asset->hashed_name) : NULL;
    asset->content_type = cwist_strdup(content_type ? content_type : guess_content_type(name));
    snprintf(asset->etag, sizeof(asset->etag), "\"%s\"", hash);

    if (!asset->name || !asset->hashed_name || !asset->url || !asset->content_type) {
        asset_free(asset);
        return NULL;
    }
    return asset;
}

static bool header_value_is_valid(const char *value) {
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p < 0x20 || *p == 0x7f) return false;
    }
    return true;
}

/** Publish `data` (owned; freed on failure) under `name`. */
static cwist_error_t asset_publish(cwist_app *app, const char *name, unsigned char *data,
                                   size_t len, const char *content_type) {
    cwist_asset_registry *reg = registry_get(app, true);
    if (!reg) {
        cwist_free(data);
        return asset_result(-1);
    }

    char hash[ASSET_HASH_HEX + 1];
    content_hash(data, len, hash);

    /* The newest asset under this name already has this content and type:
     * nothing to publish. An older match does not count, since the name
     * currently resolves to something else. */
    const char *type = content_type ? content_type : guess_content_type(name);
    for (struct cwist_asset *a = atomic_load_explicit(&reg->head, memory_order_acquire); a;
         a = a->next) {
        if (strcmp(a->name, name) != 0) continue;
        if (memcmp(a->etag + 1, hash, ASSET_HASH_HEX) == 0 && a->len == len &&
            (len == 0 || memcmp(a->data, data, len) == 0) && strcmp(a->content_type, type) == 0) {
            cwist_free(data);
            return asset_result(0);
        }
        break;
    }

    struct cwist_asset *asset = asset_build(reg, name, data, len, content_type, hash);
    if (!asset) return asset_result(-1);

    struct cwist_asset *head = atomic_load_explicit(&reg->head, memory_order_relaxed);
    do {
        asset->next = head;
    } while (!atomic_compare_exchange_weak_explicit(&reg->head, &head, asset, memory_order_release,
                                                    memory_order_relaxed));
    return asset_result(0);
}

cwist_error_t cwist_app_asset_add(cwist_app *app, const char *name, const void *data, size_t len,
                                  const char *content_type) {
    if (!app || !name_is_valid(name) || (!data && len > 0)) return asset_result(-1);
    if (content_type && (!*content_type || !header_value_is_valid(content_type))) {
        return asset_result(-1);
    }
    unsigned char *copy = (unsigned char *)cwist_alloc(len ? len : 1);
    if (!copy) return asset_result(-1);
    if (len > 0) memcpy(copy, data, len);
    return asset_publish(app, name, copy, len, content_type);
}

cwist_error_t cwist_app_asset_add_file(cwist_app *app, const char *name, const char *path,
                                       const char *content_type) {
    if (!app || !name_is_valid(name) || !path) return asset_result(-1);
    if (content_type && (!*content_type || !header_value_is_valid(content_type))) {
        return asset_result(-1);
    }
    FILE *f = fopen(path, "rb");
    if (!f) return asset_result(-1);

    size_t cap = 4096, len = 0;
    unsigned char *buf = (unsigned char *)cwist_alloc(cap);
    bool ok = buf != NULL;
    while (ok) {
        if (len == cap) {
            unsigned char *grown = (unsigned char *)cwist_realloc(buf, cap * 2);
            if (!grown) {
                ok = false;
                break;
            }
            buf = grown;
            cap *= 2;
        }
        size_t n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (n == 0) {
            ok = !ferror(f);
            break;
        }
    }
    fclose(f);
    if (!ok) {
        cwist_free(buf);
        return asset_result(-1);
    }
    return asset_publish(app, name, buf, len, content_type);
}

const char *cwist_app_asset_url(cwist_app *app, const char *name) {
    if (!app || !name) return NULL;
    cwist_asset_registry *reg = registry_get(app, false);
    if (!reg) return NULL;
    for (struct cwist_asset *a = atomic_load_explicit(&reg->head, memory_order_acquire); a;
         a = a->next) {
        if (strcmp(a->name, name) == 0) return a->url;
    }
    return NULL;
}

bool cwist_assets_match(cwist_app *app, const cwist_http_request *req, cwist_asset_match *out) {
    if (!app || !app->assets || !req || !req->path || !req->path->data) return false;
    if (req->method != CWIST_HTTP_GET && req->method != CWIST_HTTP_HEAD) return false;
    cwist_asset_registry *reg = (cwist_asset_registry *)app->assets;

    const char *path = req->path->data;
    size_t plen = strlen(reg->prefix);
    if (strncmp(path, reg->prefix, plen) != 0 || path[plen] != '/') return false;
    const char *rest = path + plen + 1;

    const struct cwist_asset *logical = NULL;
    for (struct cwist_asset *a = atomic_load_explicit(&reg->head, memory_order_acquire); a;
         a = a->next) {
        if (strcmp(a->hashed_name, rest) == 0) {
            out->asset = a;
            out->immutable = true;
            return true;
        }
        if (!logical && strcmp(a->name, rest) == 0) logical = a;
    }
    if (!logical) return false;
    out->asset = logical;
    out->immutable = false;
    return true;
}

/** Whether an If-None-Match list names `etag` (weak or strong) or is "*". */
static bool etag_matches(const char *if_none_match, const char *etag) {
    size_t elen = strlen(etag);
    const char *p = if_none_match;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',') p++;
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
        if (end - start == 1 && *start == '*') return true;
        if (end - start > 2 && start[0] == 'W' && start[1] == '/') start += 2;
        if ((size_t)(end - start) == elen && memcmp(start, etag, elen) == 0) return true;
    }
    return false;
}

static bool add_header(cwist_http_response *res, const char *name, const char *value) {
    cwist_error_t err = cwist_http_header_add(&res->headers, name, value);
    bool ok = cwist_error_is_ok(&err);
    cwist_error_dispose(&err);
    return ok;
}

void cwist_assets_respond(cwist_http_request *req, cwist_http_response *res,
                          const cwist_asset_match *match) {
    if (!req || !res) return;
    if (!match || !match->asset) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        return;
    }
    const struct cwist_asset *asset = match->asset;
    const char *cache = match->immutable ? CWIST_ASSET_IMMUTABLE_CACHE : "no-cache";
    const char *inm = cwist_http_header_get(req->headers, "If-None-Match");
    bool not_modified = inm && etag_matches(inm, asset->etag);

    bool ok = add_header(res, "ETag", asset->etag) && add_header(res, "Cache-Control", cache);
    if (ok && !not_modified) ok = add_header(res, "Content-Type", asset->content_type);
    if (!ok) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        return;
    }

    if (not_modified) {
        res->status_code = CWIST_HTTP_NOT_MODIFIED;
        cwist_error_t err = cwist_sstring_assign(res->body, "");
        cwist_error_dispose(&err);
        return;
    }
    res->status_code = CWIST_HTTP_OK;
    /* Assets live until the app is destroyed, so no release hook is needed. */
    cwist_http_response_set_body_ptr(res, asset->data, asset->len);
}

int cwist_assets_clone(void **dst, const void *src) {
    *dst = NULL;
    if (!src) return 0;
    const cwist_asset_registry *from = (const cwist_asset_registry *)src;
    cwist_asset_registry *reg = (cwist_asset_registry *)cwist_alloc(sizeof(*reg));
    if (!reg) return -1;
    reg->prefix = cwist_strdup(from->prefix);
    atomic_init(&reg->head, NULL);
    if (!reg->prefix) {
        cwist_free(reg);
        return -1;
    }

    /* Keep the newest-first order: append each copy at the tail. */
    struct cwist_asset *head = NULL, **tail = &head;
    bool ok = true;
    for (struct cwist_asset *a =
             atomic_load_explicit(&((cwist_asset_registry *)from)->head, memory_order_acquire);
         a && ok; a = a->next) {
        unsigned char *data = (unsigned char *)cwist_alloc(a->len ? a->len : 1);
        if (!data) {
            ok = false;
            break;
        }
        if (a->len) memcpy(data, a->data, a->len);
        char hash[ASSET_HASH_HEX + 1];
        memcpy(hash, a->etag + 1, ASSET_HASH_HEX);
        hash[ASSET_HASH_HEX] = '\0';
        struct cwist_asset *copy = asset_build(reg, a->name, data, a->len, a->content_type, hash);
        if (!copy) {
            ok = false;
            break;
        }
        copy->next = NULL;
        *tail = copy;
        tail = &copy->next;
    }
    atomic_init(&reg->head, head);
    if (!ok) {
        cwist_assets_destroy(reg);
        return -1;
    }
    *dst = reg;
    return 0;
}

void cwist_assets_destroy(void *registry) {
    cwist_asset_registry *reg = (cwist_asset_registry *)registry;
    if (!reg) return;
    struct cwist_asset *a = atomic_load_explicit(&reg->head, memory_order_acquire);
    while (a) {
        struct cwist_asset *next = a->next;
        asset_free(a);
        a = next;
    }
    cwist_free(reg->prefix);
    cwist_free(reg);
}
