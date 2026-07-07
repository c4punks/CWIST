/**
 * @file multipart.c
 * @brief RFC 7578 multipart/form-data parser - wrapper around multipart-parser-c.
 */

#include <cwist/net/http/multipart.h>
#include <cwist/core/mem/alloc.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>
#include "multipart_parser.h"

typedef struct {
    char header_field[256];
    size_t header_field_len;
    char header_value[1024];
    size_t header_value_len;
    /* Set once a value was delivered for the current header, even an empty
     * one; header_value_len alone cannot express "seen but empty". */
    bool have_value;

    char name[256];
    char filename[256];
    char content_type[256];

    char *data;
    size_t data_len;
    size_t data_cap;

    cwist_multipart_result *result;
} mp_parse_ctx;

static void mp_parse_headers(mp_parse_ctx *ctx);

static int mp_on_header_field(multipart_parser *p, const char *at, size_t len) {
    mp_parse_ctx *ctx = (mp_parse_ctx *)multipart_parser_get_data(p);
    if (ctx->header_value_len > 0) {
        /* Previous header value is complete, parse it before moving to next field. */
        mp_parse_headers(ctx);
    }
    if (ctx->header_field_len + len < sizeof(ctx->header_field)) {
        memcpy(ctx->header_field + ctx->header_field_len, at, len);
        ctx->header_field_len += len;
    }
    return 0;
}

/**
 * @brief multipart-parser-c callback: header value fragment received.
 *
 * Marks the current header as having a value and appends the fragment to the
 * buffered header value (truncated at the fixed buffer size).
 *
 * @param p Parser instance (carries mp_parse_ctx).
 * @param at Pointer to the fragment.
 * @param len Fragment length.
 * @return 0 on success.
 */
static int mp_on_header_value(multipart_parser *p, const char *at, size_t len) {
    mp_parse_ctx *ctx = (mp_parse_ctx *)multipart_parser_get_data(p);
    ctx->have_value = true;
    if (ctx->header_value_len + len < sizeof(ctx->header_value)) {
        memcpy(ctx->header_value + ctx->header_value_len, at, len);
        ctx->header_value_len += len;
    }
    return 0;
}

/**
 * @brief Finalize the buffered header field/value pair into ctx state.
 *
 * NUL-terminates both buffers (truncating at their fixed sizes), then, for
 * "Content-Disposition", extracts the name="..." and filename="..." tags, and
 * for "Content-Type" stores the whole value. Not thread-safe: operates on the
 * caller's ctx. Resets the field/value buffers and have_value afterwards.
 *
 * @param ctx Parse context to update in place.
 */
static void mp_parse_headers(mp_parse_ctx *ctx) {
    if (ctx->header_field_len >= sizeof(ctx->header_field))
        ctx->header_field_len = sizeof(ctx->header_field) - 1;
    ctx->header_field[ctx->header_field_len] = '\0';

    if (ctx->header_value_len >= sizeof(ctx->header_value))
        ctx->header_value_len = sizeof(ctx->header_value) - 1;
    ctx->header_value[ctx->header_value_len] = '\0';

    if (strcasecmp(ctx->header_field, "Content-Disposition") == 0) {
        const char *name_tag = strstr(ctx->header_value, "name=\"");
        if (name_tag) {
            name_tag += 6;
            const char *name_end = strchr(name_tag, '"');
            if (name_end) {
                size_t nlen = (size_t)(name_end - name_tag);
                if (nlen < sizeof(ctx->name)) {
                    memcpy(ctx->name, name_tag, nlen);
                    ctx->name[nlen] = '\0';
                }
            }
        }
        const char *filename_tag = strstr(ctx->header_value, "filename=\"");
        if (filename_tag) {
            filename_tag += 10;
            const char *filename_end = strchr(filename_tag, '"');
            if (filename_end) {
                size_t flen = (size_t)(filename_end - filename_tag);
                if (flen < sizeof(ctx->filename)) {
                    memcpy(ctx->filename, filename_tag, flen);
                    ctx->filename[flen] = '\0';
                }
            }
        }
    } else if (strcasecmp(ctx->header_field, "Content-Type") == 0) {
        size_t vlen = strlen(ctx->header_value);
        if (vlen < sizeof(ctx->content_type)) strcpy(ctx->content_type, ctx->header_value);
    }

    ctx->header_field_len = 0;
    ctx->header_value_len = 0;
    ctx->have_value = false;
}

/**
 * @brief multipart-parser-c callback: all headers of a part were received.
 *
 * Flushes any pending header pair via mp_parse_headers() so its values are
 * visible before part data begins.
 *
 * @param p Parser instance (carries mp_parse_ctx).
 * @return 0 on success.
 */
static int mp_on_headers_complete(multipart_parser *p) {
    mp_parse_ctx *ctx = (mp_parse_ctx *)multipart_parser_get_data(p);
    mp_parse_headers(ctx);
    return 0;
}

/**
 * @brief multipart-parser-c callback: start of a new part.
 *
 * Flushes any pending header pair, resets header/name/filename/content_type
 * buffers and the data accumulator. Any buffered ctx->data from a previous
 * part is freed here; ownership of the current accumulation passes to the
 * cwist_multipart_field created in mp_on_part_data_end.
 *
 * @param p Parser instance (carries mp_parse_ctx).
 * @return 0 on success.
 */
static int mp_on_part_data_begin(multipart_parser *p) {
    mp_parse_ctx *ctx = (mp_parse_ctx *)multipart_parser_get_data(p);
    if (ctx->header_value_len > 0) {
        mp_parse_headers(ctx);
    }
    ctx->header_field_len = 0;
    ctx->header_value_len = 0;
    ctx->have_value = false;
    ctx->name[0] = '\0';
    ctx->filename[0] = '\0';
    ctx->content_type[0] = '\0';
    if (ctx->data) {
        cwist_free(ctx->data);
        ctx->data = NULL;
    }
    ctx->data_len = 0;
    ctx->data_cap = 0;
    return 0;
}

/**
 * @brief multipart-parser-c callback: part body fragment received.
 *
 * Appends the fragment to ctx->data, growing the buffer with cwist_alloc as
 * needed (doubling, minimum 256 bytes). The old buffer is freed on growth.
 *
 * @param p Parser instance (carries mp_parse_ctx).
 * @param at Pointer to the fragment.
 * @param len Fragment length.
 * @return 0 on success, 1 if memory allocation failed (aborts parsing).
 */
static int mp_on_part_data(multipart_parser *p, const char *at, size_t len) {
    mp_parse_ctx *ctx = (mp_parse_ctx *)multipart_parser_get_data(p);
    if (len == 0) return 0;
    if (ctx->data_len + len > ctx->data_cap) {
        size_t new_cap = (ctx->data_len + len) * 2;
        if (new_cap < 256) new_cap = 256;
        char *new_data = (char *)cwist_alloc(new_cap);
        if (!new_data) return 1;
        if (ctx->data) {
            memcpy(new_data, ctx->data, ctx->data_len);
            cwist_free(ctx->data);
        }
        ctx->data = new_data;
        ctx->data_cap = new_cap;
    }
    memcpy(ctx->data + ctx->data_len, at, len);
    ctx->data_len += len;
    return 0;
}

/**
 * @brief multipart-parser-c callback: end of a part.
 *
 * Allocates a cwist_multipart_field, copies name/filename/content_type into
 * freshly allocated strings, and transfers ownership of the accumulated
 * ctx->data buffer to the field (no copy). The field is prepended to
 * ctx->result->fields and the accumulation state is reset to empty.
 *
 * @param p Parser instance (carries mp_parse_ctx).
 * @return 0 on success, 1 if memory allocation failed (aborts parsing).
 */
static int mp_on_part_data_end(multipart_parser *p) {
    mp_parse_ctx *ctx = (mp_parse_ctx *)multipart_parser_get_data(p);
    cwist_multipart_field *field =
        (cwist_multipart_field *)cwist_alloc(sizeof(cwist_multipart_field));
    if (!field) return 1;
    memset(field, 0, sizeof(*field));

    if (ctx->name[0]) {
        field->name = (char *)cwist_alloc(strlen(ctx->name) + 1);
        if (field->name) strcpy(field->name, ctx->name);
    }
    if (ctx->filename[0]) {
        field->filename = (char *)cwist_alloc(strlen(ctx->filename) + 1);
        if (field->filename) strcpy(field->filename, ctx->filename);
    }
    if (ctx->content_type[0]) {
        field->content_type = (char *)cwist_alloc(strlen(ctx->content_type) + 1);
        if (field->content_type) strcpy(field->content_type, ctx->content_type);
    }
    field->data = ctx->data;
    field->data_len = ctx->data_len;
    field->next = ctx->result->fields;
    ctx->result->fields = field;

    ctx->data = NULL;
    ctx->data_len = 0;
    ctx->data_cap = 0;
    return 0;
}

/**
 * @brief Parse a multipart/form-data body into a field list.
 *
 * Runs the multipart-parser-c state machine over @p body with @p boundary
 * (the boundary without leading dashes). On malformed or truncated input the
 * parser stops early and NULL is returned, with all intermediate state freed.
 *
 * @param body Raw body bytes; must not be NULL.
 * @param body_len Length of @p body in bytes.
 * @param boundary MIME boundary string, without the leading "--".
 * @return Newly allocated cwist_multipart_result on success (caller owns it,
 *         release with cwist_multipart_result_destroy), NULL on invalid
 *         arguments, malformed input, or allocation failure.
 */
cwist_multipart_result *cwist_multipart_parse(const char *body, size_t body_len,
                                              const char *boundary) {
    if (!body || body_len == 0 || !boundary) return NULL;

    cwist_multipart_result *result =
        (cwist_multipart_result *)cwist_alloc(sizeof(cwist_multipart_result));
    if (!result) return NULL;
    result->fields = NULL;

    mp_parse_ctx ctx = {0};
    ctx.result = result;

    multipart_parser_settings settings = {
        .on_header_field = mp_on_header_field,
        .on_header_value = mp_on_header_value,
        .on_headers_complete = mp_on_headers_complete,
        .on_part_data_begin = mp_on_part_data_begin,
        .on_part_data = mp_on_part_data,
        .on_part_data_end = mp_on_part_data_end,
    };

    /* multipart-parser-c expects the leading dashes in the boundary. */
    size_t blen = strlen(boundary);
    char *parser_boundary = (char *)cwist_alloc(blen + 3);
    if (!parser_boundary) {
        cwist_free(result);
        return NULL;
    }
    memcpy(parser_boundary, "--", 2);
    memcpy(parser_boundary + 2, boundary, blen);
    parser_boundary[blen + 2] = '\0';

    multipart_parser *parser = multipart_parser_init(parser_boundary, &settings);
    cwist_free(parser_boundary);
    if (!parser) {
        cwist_free(result);
        return NULL;
    }
    multipart_parser_set_data(parser, &ctx);
    size_t consumed = multipart_parser_execute(parser, body, body_len);
    multipart_parser_free(parser);

    /* Truncated body: on_part_data_end never fired, so release the in-flight
     * part buffer here. */
    if (ctx.data) {
        cwist_free(ctx.data);
        ctx.data = NULL;
    }

    /* The parser stops early on malformed input; report it as NULL per the
     * documented contract. */
    if (consumed != body_len) {
        cwist_multipart_result_destroy(result);
        return NULL;
    }

    return result;
}

/**
 * @brief Destroy a parse result and everything it owns.
 *
 * Frees every field's name, filename, content_type, and data, then the field
 * nodes and the result itself. Safe to call with NULL.
 *
 * @param result Result returned by cwist_multipart_parse, or NULL.
 */
void cwist_multipart_result_destroy(cwist_multipart_result *result) {
    if (!result) return;
    cwist_multipart_field *curr = result->fields;
    while (curr) {
        cwist_multipart_field *next = curr->next;
        cwist_free(curr->name);
        cwist_free(curr->filename);
        cwist_free(curr->content_type);
        cwist_free(curr->data);
        cwist_free(curr);
        curr = next;
    }
    cwist_free(result);
}

/**
 * @brief Extract the boundary parameter from a Content-Type header value.
 *
 * Scans semicolon-separated parameters of @p content_type, skipping optional
 * whitespace, and accepts both quoted and unquoted boundary values.
 *
 * @param content_type Content-Type header value, e.g. from the request; may be
 *                     NULL.
 * @return Newly allocated NUL-terminated boundary string (caller frees with
 *         cwist_free), or NULL if no boundary parameter is present or
 *         allocation failed.
 */
char *cwist_multipart_extract_boundary(const char *content_type) {
    if (!content_type) return NULL;
    const char *p = content_type;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ';')) p++;
        if (strncasecmp(p, "boundary", 8) == 0) {
            p += 8;
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p == '=') {
                p++;
                while (*p && isspace((unsigned char)*p)) p++;
                const char *start = p;
                if (*p == '"') {
                    start = ++p;
                    while (*p && *p != '"') p++;
                } else {
                    while (*p && *p != ';' && !isspace((unsigned char)*p)) p++;
                }
                size_t len = (size_t)(p - start);
                char *boundary = (char *)cwist_alloc(len + 1);
                if (boundary) {
                    memcpy(boundary, start, len);
                    boundary[len] = '\0';
                }
                return boundary;
            }
        }
        while (*p && *p != ';') p++;
        if (*p == ';') p++;
    }
    return NULL;
}
