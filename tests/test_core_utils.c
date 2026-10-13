/**
 * @file test_core_utils.c
 * @brief Unit tests for the remaining sstring entry points, the query map
 * (heap and arena), the CSS composer's color/variables/utility output and
 * the plain allocator helpers.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/core/html/css_composer.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/mem/arena.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/net/http/http.h>
#include <cwist/net/http/query.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool ok(cwist_error_t e) {
    return e.errtype == CWIST_ERR_INT8 && e.error.err_i8 == ERR_SSTRING_OKAY;
}

/* --- sstring ------------------------------------------------------------- */

static void test_sstring_copy_append_trim(void) {
    cwist_sstring *a = cwist_sstring_create();
    cwist_sstring *b = cwist_sstring_create();
    assert(ok(cwist_sstring_assign(a, "hello")));
    assert(ok(cwist_sstring_assign(b, " world")));

    assert(ok(cwist_sstring_append_sstring(a, b)));
    assert(strcmp(a->data, "hello world") == 0 && a->size == 11);
    assert(ok(cwist_sstring_append_sstring(a, NULL))); /* NULL source: no-op */
    assert(cwist_sstring_append_sstring(NULL, b).error.err_i8 == ERR_SSTRING_NULL_STRING);

    assert(ok(cwist_sstring_copy_sstring(b, a)));
    assert(strcmp(b->data, "hello world") == 0);
    assert(ok(cwist_sstring_copy_sstring(b, NULL)));
    assert(strcmp(b->data, "") == 0);

    char out[32];
    assert(ok(cwist_sstring_copy(a, out)));
    assert(strcmp(out, "hello world") == 0);
    assert(cwist_sstring_copy(a, NULL).error.err_i8 == ERR_SSTRING_NULL_STRING);
    assert(cwist_sstring_copy(NULL, out).error.err_i8 == ERR_SSTRING_NULL_STRING);

    assert(ok(cwist_sstring_assign(b, " \t\n  padded ")));
    assert(ok(cwist_sstring_ltrim(b)));
    assert(strcmp(b->data, "padded ") == 0 && b->size == 7);
    assert(ok(cwist_sstring_assign(b, "   ")));
    assert(ok(cwist_sstring_ltrim(b)));
    assert(strcmp(b->data, "") == 0 && b->size == 0);
    assert(ok(cwist_sstring_assign(b, "none")));
    assert(ok(cwist_sstring_ltrim(b)));
    assert(strcmp(b->data, "none") == 0);
    cwist_sstring_destroy(a);
    cwist_sstring_destroy(b);
}

/* Appending or assigning a string to itself used to read the source after
 * growth had freed it (and strcat/strcpy on overlapping memory otherwise).
 * Run under ASan to see the old failure. */
static void test_sstring_self_alias(void) {
    cwist_sstring *s = cwist_sstring_create();
    assert(ok(cwist_sstring_assign(s, "abc")));
    for (int i = 0; i < 6; i++) assert(ok(cwist_sstring_append_sstring(s, s)));
    assert(s->size == 3u << 6 && strlen(s->data) == s->size);
    for (size_t i = 0; i < s->size; i++) assert(s->data[i] == "abc"[i % 3]);

    assert(ok(cwist_sstring_assign(s, "tail-of-me")));
    assert(ok(cwist_sstring_append(s, s->data + 5))); /* pointer into itself */
    assert(strcmp(s->data, "tail-of-meof-me") == 0);
    assert(ok(cwist_sstring_assign(s, s->data + 5))); /* overlapping assign */
    assert(strcmp(s->data, "of-meof-me") == 0);
    assert(ok(cwist_sstring_copy_sstring(s, s)));
    assert(strcmp(s->data, "of-meof-me") == 0);

    assert(ok(cwist_sstring_assign(s, "<a>")));
    assert(ok(cwist_sstring_append_escaped(s, s->data)));
    assert(strcmp(s->data, "<a>&lt;a&gt;") == 0);
    cwist_sstring_destroy(s);
}

static void test_sstring_escaped_and_adopt(void) {
    cwist_sstring s;
    assert(ok(cwist_sstring_init_escaped(&s)));
    assert(s.data == NULL && s.size == 0);
    /* The escaped variant's method table escapes on append. */
    cwist_sstring src;
    assert(ok(cwist_sstring_init(&src)));
    assert(ok(cwist_sstring_assign(&src, "<b>\"x\" & 'y'</b>")));
    assert(ok(s.append(&s, &src)));
    assert(strcmp(s.data, "&lt;b&gt;&quot;x&quot; &amp; &#39;y&#39;&lt;/b&gt;") == 0);
    assert(s.size == strlen(s.data));
    assert(ok(cwist_sstring_append_sstring_escaped(&s, NULL)));
    assert(cwist_sstring_append_sstring_escaped(NULL, &src).error.err_i8 == ERR_SSTRING_NULL_STRING);
    assert(cwist_sstring_init_escaped(NULL).error.err_i8 == ERR_SSTRING_NULL_STRING);

    /* adopt_len takes ownership of a heap buffer and NUL-terminates it. */
    char *buf = cwist_malloc(16);
    memcpy(buf, "adoptedXXXX", 11);
    assert(ok(cwist_sstring_adopt_len(&s, buf, 7)));
    assert(s.data == buf && s.size == 7 && strcmp(s.data, "adopted") == 0);
    assert(ok(cwist_sstring_append(&s, "!")));
    assert(strcmp(s.data, "adopted!") == 0);
    assert(ok(cwist_sstring_adopt_len(&s, NULL, 5)));
    assert(s.data == NULL && s.size == 0);
    assert(cwist_sstring_adopt_len(NULL, NULL, 0).error.err_i8 == ERR_SSTRING_NULL_STRING);
    cwist_free(src.data);
}

/* --- query map ----------------------------------------------------------- */

typedef struct {
    int count;
    char seen[256];
} iter_ctx;

static void collect(const char *k, const char *v, void *p) {
    iter_ctx *c = p;
    c->count++;
    strcat(c->seen, k);
    strcat(c->seen, "=");
    strcat(c->seen, v);
    strcat(c->seen, ";");
}

static void exercise_query_map(cwist_query_map *m) {
    cwist_query_map_parse(m, "a=1&b=two%20words&c=&a=ignored&noeq&d=%26");
    assert(strcmp(cwist_query_map_get(m, "a"), "1") == 0); /* first value wins */
    assert(strcmp(cwist_query_map_get(m, "b"), "two words") == 0);
    assert(strcmp(cwist_query_map_get(m, "c"), "") == 0);
    assert(strcmp(cwist_query_map_get(m, "d"), "&") == 0);
    assert(cwist_query_map_get(m, "noeq") == NULL);

    cwist_query_map_set(m, "a", "replaced");
    cwist_query_map_set(m, "e", "5");
    cwist_query_map_set(m, NULL, "x");
    cwist_query_map_set(m, "f", NULL);
    assert(strcmp(cwist_query_map_get(m, "a"), "replaced") == 0);
    assert(cwist_query_map_get(m, "f") == NULL);

    iter_ctx c = {0};
    cwist_query_map_foreach(m, collect, &c);
    assert(c.count == 5);
    assert(strstr(c.seen, "a=replaced;") && strstr(c.seen, "e=5;") && strstr(c.seen, "c=;"));
    cwist_query_map_foreach(m, NULL, &c);
    cwist_query_map_foreach(NULL, collect, &c);

    cwist_query_map_delete(m, "a");
    cwist_query_map_delete(m, "missing");
    cwist_query_map_delete(m, NULL);
    assert(cwist_query_map_get(m, "a") == NULL);
    assert(strcmp(cwist_query_map_get(m, "e"), "5") == 0);

    /* Many keys chain within the fixed buckets. */
    char k[16];
    for (int i = 0; i < 300; i++) {
        snprintf(k, sizeof(k), "k%d", i);
        cwist_query_map_set(m, k, k);
    }
    for (int i = 0; i < 300; i += 7) {
        snprintf(k, sizeof(k), "k%d", i);
        cwist_query_map_delete(m, k);
    }
    for (int i = 0; i < 300; i++) {
        snprintf(k, sizeof(k), "k%d", i);
        const char *v = cwist_query_map_get(m, k);
        if (i % 7 == 0) assert(v == NULL);
        else assert(v && strcmp(v, k) == 0);
    }

    cwist_query_map_clear(m);
    c = (iter_ctx){0};
    cwist_query_map_foreach(m, collect, &c);
    assert(c.count == 0);
    cwist_query_map_set(m, "after", "clear"); /* still usable */
    assert(strcmp(cwist_query_map_get(m, "after"), "clear") == 0);
    cwist_query_map_clear(NULL);
}

static void test_query_map_heap_and_arena(void) {
    cwist_query_map *m = cwist_query_map_create();
    assert(m && m->arena == NULL);
    exercise_query_map(m);
    cwist_query_map_destroy(m);

    cwist_arena_t *arena = cwist_arena_create(0);
    assert(arena);
    m = cwist_query_map_create_in_arena(arena);
    assert(m && m->arena == arena);
    assert(cwist_arena_owns(arena, m));
    exercise_query_map(m);
    /* The 300 keys overflow the 8 KiB arena, so later entries live on the
     * heap; destroy frees those and leaves arena memory to the arena. */
    cwist_query_map_destroy(m);
    cwist_arena_destroy(arena);
}

/* Malformed percent-escapes must behave exactly like the old
 * sscanf/isxdigit implementation: a '%' not followed by two hex digits is
 * copied literally and scanning resumes at the next byte. */
static void test_query_map_malformed_escapes(void) {
    cwist_query_map *m = cwist_query_map_create();
    assert(m);

    cwist_query_map_parse(m,
                          "lit=%zz&trunc=%4&lonely=%&ok=%41&"
                          "mixed=a%4zb&lower=%ab&upper=%AB&plus=a+b&trail=x%");
    assert(strcmp(cwist_query_map_get(m, "lit"), "%zz") == 0);
    assert(strcmp(cwist_query_map_get(m, "trunc"), "%4") == 0);
    assert(strcmp(cwist_query_map_get(m, "lonely"), "%") == 0);
    assert(strcmp(cwist_query_map_get(m, "ok"), "A") == 0);
    assert(strcmp(cwist_query_map_get(m, "mixed"), "a%4zb") == 0);
    assert(strcmp(cwist_query_map_get(m, "lower"), "\xab") == 0);
    assert(strcmp(cwist_query_map_get(m, "upper"), "\xab") == 0);
    assert(strcmp(cwist_query_map_get(m, "plus"), "a b") == 0);
    assert(strcmp(cwist_query_map_get(m, "trail"), "x%") == 0);

    /* Consecutive delimiters and a trailing '&' collapse like strtok_r. */
    cwist_query_map_clear(m);
    cwist_query_map_parse(m, "&&a=1&&&b=2&");
    assert(strcmp(cwist_query_map_get(m, "a"), "1") == 0);
    assert(strcmp(cwist_query_map_get(m, "b"), "2") == 0);

    cwist_query_map_destroy(m);
}

/* A request's query map lives in its 8 KiB arena. A query of ~1.4 KiB
 * (80 params) used to exhaust it, and every later parameter was dropped
 * without an error. */
static void test_query_map_arena_large_query(void) {
    static char q[64 * 1024];
    size_t off = 0;
    const int n = 2000;
    for (int i = 0; i < n; i++)
        off += (size_t)snprintf(q + off, sizeof(q) - off, "%sparam%04d=value%04d", i ? "&" : "", i, i);
    cwist_arena_t *arena = cwist_arena_create(0);
    cwist_query_map *m = cwist_query_map_create_in_arena(arena);
    cwist_query_map_parse(m, q);
    char k[32], v[32];
    for (int i = 0; i < n; i++) {
        snprintf(k, sizeof(k), "param%04d", i);
        snprintf(v, sizeof(v), "value%04d", i);
        const char *got = cwist_query_map_get(m, k);
        assert(got && strcmp(got, v) == 0);
    }
    cwist_query_map_destroy(m);
    cwist_arena_destroy(arena);
}

/* The same through the real request parser, whose request arena also holds
 * the path, headers and other request state. */
static void test_query_params_through_request_parser(void) {
    static char raw[16 * 1024];
    size_t off = (size_t)snprintf(raw, sizeof(raw), "GET /search?");
    const int n = 300;
    for (int i = 0; i < n; i++)
        off += (size_t)snprintf(raw + off, sizeof(raw) - off, "%sp%03d=v%03d", i ? "&" : "", i, i);
    snprintf(raw + off, sizeof(raw) - off, " HTTP/1.1\r\nHost: x\r\nUser-Agent: t\r\n\r\n");
    cwist_http_request *req = cwist_http_parse_request(raw);
    assert(req && req->query_params);
    char k[16], v[16];
    for (int i = 0; i < n; i++) {
        snprintf(k, sizeof(k), "p%03d", i);
        snprintf(v, sizeof(v), "v%03d", i);
        const char *got = cwist_query_map_get(req->query_params, k);
        assert(got && strcmp(got, v) == 0);
    }
    cwist_http_request_destroy(req);
}

/* The map used to keep 16 buckets forever; it now doubles past a 3/4 load
 * factor, on the heap and in an arena alike, and keeps count exact. */
static size_t longest_chain(const cwist_query_map *m) {
    size_t worst = 0;
    for (size_t i = 0; i < m->size; i++) {
        size_t n = 0;
        for (cwist_query_bucket *b = m->buckets[i]; b; b = b->next) n++;
        if (n > worst) worst = n;
    }
    return worst;
}

static void exercise_growth(cwist_query_map *m) {
    char k[16];
    for (int i = 0; i < 1000; i++) {
        snprintf(k, sizeof(k), "g%d", i);
        cwist_query_map_set(m, k, k);
    }
    assert(m->count == 1000);
    assert(m->size >= 1024 && m->count <= m->size - m->size / 4);
    assert(longest_chain(m) < 16); /* 1000 keys in 16 buckets averaged 62 */
    cwist_query_map_set(m, "g7", "updated"); /* update: count unchanged */
    assert(m->count == 1000);
    for (int i = 0; i < 1000; i++) {
        snprintf(k, sizeof(k), "g%d", i);
        assert(cwist_query_map_get(m, k));
    }
    for (int i = 0; i < 1000; i += 2) {
        snprintf(k, sizeof(k), "g%d", i);
        cwist_query_map_delete(m, k);
    }
    assert(m->count == 500);
    cwist_query_map_clear(m);
    assert(m->count == 0);
}

static void test_query_map_growth(void) {
    cwist_query_map *m = cwist_query_map_create();
    assert(m->size == 16 && m->count == 0);
    exercise_growth(m);
    cwist_query_map_destroy(m);

    cwist_arena_t *arena = cwist_arena_create(0);
    m = cwist_query_map_create_in_arena(arena);
    exercise_growth(m); /* grown bucket arrays fall back to the heap */
    cwist_query_map_destroy(m);
    cwist_arena_destroy(arena);

    /* A zeroed map (size 0) must not divide by zero; set grows it. */
    cwist_query_map z;
    memset(&z, 0, sizeof(z));
    assert(cwist_query_map_get(&z, "a") == NULL);
    cwist_query_map_delete(&z, "a");
    cwist_query_map_set(&z, "a", "1");
    assert(z.size > 0 && z.count == 1 && strcmp(cwist_query_map_get(&z, "a"), "1") == 0);
    cwist_query_map_clear(&z);
    cwist_free(z.buckets);
}

/* --- css composer -------------------------------------------------------- */

static void expect_rgb(cwist_color_rgb c, int r, int g, int b) {
    assert(c.r == r && c.g == g && c.b == b);
}

static void test_css_colors(void) {
    expect_rgb(cwist_color_hsl_to_rgb((cwist_color_hsl){0, 1, 0.5f}), 255, 0, 0);
    expect_rgb(cwist_color_hsl_to_rgb((cwist_color_hsl){120, 1, 0.5f}), 0, 255, 0);
    expect_rgb(cwist_color_hsl_to_rgb((cwist_color_hsl){240, 1, 0.5f}), 0, 0, 255);
    expect_rgb(cwist_color_hsl_to_rgb((cwist_color_hsl){0, 0, 1}), 255, 255, 255);
    expect_rgb(cwist_color_hsl_to_rgb((cwist_color_hsl){0, 0, 0}), 0, 0, 0);
    /* Gray rounds like the chromatic channels: 0.5 -> 128, not 127. */
    expect_rgb(cwist_color_hsl_to_rgb((cwist_color_hsl){0, 0, 0.5f}), 128, 128, 128);
    /* Hue wraps: 360 and 720 are red, -120 is blue. */
    expect_rgb(cwist_color_hsl_to_rgb((cwist_color_hsl){360, 1, 0.5f}), 255, 0, 0);
    expect_rgb(cwist_color_hsl_to_rgb((cwist_color_hsl){720, 1, 0.5f}), 255, 0, 0);
    expect_rgb(cwist_color_hsl_to_rgb((cwist_color_hsl){-120, 1, 0.5f}), 0, 0, 255);
    /* Out-of-range saturation/lightness clamp instead of overflowing. */
    expect_rgb(cwist_color_hsl_to_rgb((cwist_color_hsl){0, 0, 2.0f}), 255, 255, 255);
    expect_rgb(cwist_color_hsl_to_rgb((cwist_color_hsl){0, 3.0f, -1.0f}), 0, 0, 0);

    /* rgb -> hsl -> rgb round-trips. */
    const char *hex[] = {"#3B82F6", "#10B981", "#123456", "#FFFFFF", "#000000", "#808080"};
    for (size_t i = 0; i < sizeof(hex) / sizeof(*hex); i++) {
        cwist_color_rgb c = cwist_color_hex_to_rgb(hex[i]);
        cwist_color_rgb back = cwist_color_hsl_to_rgb(cwist_color_rgb_to_hsl(c));
        assert(abs(back.r - c.r) <= 1 && abs(back.g - c.g) <= 1 && abs(back.b - c.b) <= 1);
    }
}

static void test_css_generators(void) {
    cwist_css_config cfg;
    cwist_css_config_init(&cfg);
    cwist_sstring *vars = cwist_css_generate_variables(&cfg);
    assert(vars && vars->data);
    assert(strncmp(vars->data, ":root {\n", 8) == 0);
    assert(strstr(vars->data, "--color-primary: #3B82F6;") || strstr(vars->data, "--color-primary: #3b82f6;"));
    assert(strstr(vars->data, "--bg-body: #FFFFFF;"));
    assert(strstr(vars->data, "--radius-md: 8.0px;"));
    assert(strstr(vars->data, "--radius-lg: 12.0px;"));
    assert(strstr(vars->data, "--space-8: 32.0px;"));
    cwist_sstring_destroy(vars);

    cfg.is_dark_mode = true;
    cfg.roundness_px = 2.0f;
    vars = cwist_css_generate_variables(&cfg);
    assert(strstr(vars->data, "--bg-body: #121212;"));
    assert(strstr(vars->data, "--radius-sm: 1.0px;"));
    cwist_sstring_destroy(vars);
    assert(cwist_css_generate_variables(NULL) == NULL);

    cwist_sstring *utils = cwist_css_generate_utility_classes(&cfg);
    assert(utils && strstr(utils->data, ".btn {") && strstr(utils->data, ".card {"));
    assert(strstr(utils->data, "var(--color-primary-hover)"));
    cwist_sstring_destroy(utils);
    utils = cwist_css_generate_utility_classes(NULL); /* cfg is not used */
    assert(utils && strstr(utils->data, ".btn {"));
    cwist_sstring_destroy(utils);
}

/* --- allocator helpers --------------------------------------------------- */

static void test_alloc_helpers(void) {
    char *p = cwist_malloc(32);
    assert(p);
    memset(p, 'x', 32);
    cwist_free(p);

    char *d = cwist_strndup("abcdef", 3);
    assert(d && strcmp(d, "abc") == 0);
    cwist_free(d);
    d = cwist_strndup("ab", 10); /* n past the end copies the whole string */
    assert(d && strcmp(d, "ab") == 0);
    cwist_free(d);
    d = cwist_strndup("abc", 0);
    assert(d && d[0] == '\0');
    cwist_free(d);
    assert(cwist_strndup(NULL, 3) == NULL);
}

int main(void) {
    test_sstring_copy_append_trim();
    test_sstring_self_alias();
    test_sstring_escaped_and_adopt();
    test_query_map_heap_and_arena();
    test_query_map_arena_large_query();
    test_query_map_malformed_escapes();
    test_query_params_through_request_parser();
    test_query_map_growth();
    test_css_colors();
    test_css_generators();
    test_alloc_helpers();
    printf("test_core_utils: ok\n");
    return 0;
}
