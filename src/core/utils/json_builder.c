#include <cwist/json_builder.h>
#include <cwist/sstring.h>
#include <stdlib.h>
#include <stdio.h>

cwist_json_builder *cwist_json_builder_create(void) {
    cwist_json_builder *b = (cwist_json_builder *)malloc(sizeof(cwist_json_builder));
    if (!b) return NULL;
    b->buffer = cwist_sstring_create();
    b->needs_comma = false;
    return b;
}

void cwist_json_builder_destroy(cwist_json_builder *b) {
    if (b) {
        cwist_sstring_destroy(b->buffer);
        free(b);
    }
}

static void append_comma_if_needed(cwist_json_builder *b) {
    if (b->needs_comma) {
        cwist_sstring_append(b->buffer, ",");
    }
}

static void append_json_escaped_str(cwist_sstring *buf, const char *str) {
    if (!str) return;
    cwist_sstring_append(buf, "\"");
    const char *start = str;
    const char *p = str;
    while (*p) {
        const char *esc = NULL;
        char ucode[8];
        switch (*p) {
            case '"': esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\b': esc = "\\b"; break;
            case '\f': esc = "\\f"; break;
            case '\n': esc = "\\n"; break;
            case '\r': esc = "\\r"; break;
            case '\t': esc = "\\t"; break;
            default:
                if ((unsigned char)*p < 0x20) {
                    snprintf(ucode, sizeof(ucode), "\\u%04x", (unsigned char)*p);
                    esc = ucode;
                }
                break;
        }
        if (esc) {
            if (p > start) {
                char tmp[4096];
                size_t seg_len = (size_t)(p - start);
                while (seg_len > 0) {
                    size_t chunk = seg_len < sizeof(tmp) - 1 ? seg_len : sizeof(tmp) - 1;
                    memcpy(tmp, start, chunk);
                    tmp[chunk] = '\0';
                    cwist_sstring_append(buf, tmp);
                    start += chunk;
                    seg_len -= chunk;
                }
            }
            cwist_sstring_append(buf, esc);
            start = p + 1;
        }
        p++;
    }
    if (p > start) {
        cwist_sstring_append(buf, start);
    }
    cwist_sstring_append(buf, "\"");
}

static void append_key_if_present(cwist_json_builder *b, const char *key) {
    if (key) {
        append_json_escaped_str(b->buffer, key);
        cwist_sstring_append(b->buffer, ":");
    }
}

/**
 * @brief Begin a JSON object in the current builder context.
 * @param b Builder to update. NULL is ignored.
 */
void cwist_json_begin_object(cwist_json_builder *b) {
    if (!b) return;
    append_comma_if_needed(b);
    cwist_sstring_append(b->buffer, "{");
    b->needs_comma = false;
}

void cwist_json_end_object(cwist_json_builder *b) {
    if (!b) return;
    cwist_sstring_append(b->buffer, "}");
    b->needs_comma = true;
}

void cwist_json_begin_array(cwist_json_builder *b, const char *key) {
    if (!b) return;
    append_comma_if_needed(b);
    if (key) {
        cwist_sstring_append(b->buffer, "\"");
        cwist_sstring_append(b->buffer, (char*)key);
        cwist_sstring_append(b->buffer, "\":[");
    } else {
        cwist_sstring_append(b->buffer, "[");
    }
    b->needs_comma = false;
}

void cwist_json_end_array(cwist_json_builder *b) {
    if (!b) return;
    cwist_sstring_append(b->buffer, "]");
    b->needs_comma = true;
}

void cwist_json_add_string(cwist_json_builder *b, const char *key, const char *value) {
    if (!b) return;
    append_comma_if_needed(b);
    if (key) {
        cwist_sstring_append(b->buffer, "\"");
        cwist_sstring_append(b->buffer, (char*)key);
        cwist_sstring_append(b->buffer, "\":");
    }
    cwist_sstring_append(b->buffer, "\"");
    cwist_sstring_append(b->buffer, (char*)value);
    cwist_sstring_append(b->buffer, "\"");
    b->needs_comma = true;
}

void cwist_json_add_int(cwist_json_builder *b, const char *key, int value) {
    if (!b) return;
    append_comma_if_needed(b);
    if (key) {
        cwist_sstring_append(b->buffer, "\"");
        cwist_sstring_append(b->buffer, (char*)key);
        cwist_sstring_append(b->buffer, "\":");
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    cwist_sstring_append(b->buffer, buf);
    b->needs_comma = true;
}

void cwist_json_add_bool(cwist_json_builder *b, const char *key, bool value) {
    if (!b) return;
    append_comma_if_needed(b);
    if (key) {
        cwist_sstring_append(b->buffer, "\"");
        cwist_sstring_append(b->buffer, (char*)key);
        cwist_sstring_append(b->buffer, "\":");
    }
    cwist_sstring_append(b->buffer, value ? "true" : "false");
    b->needs_comma = true;
}

void cwist_json_add_null(cwist_json_builder *b, const char *key) {
    if (!b) return;
    append_comma_if_needed(b);
    if (key) {
        cwist_sstring_append(b->buffer, "\"");
        cwist_sstring_append(b->buffer, (char*)key);
        cwist_sstring_append(b->buffer, "\":");
    }
    cwist_sstring_append(b->buffer, "null");
    b->needs_comma = true;
}

const char *cwist_json_get_raw(cwist_json_builder *b) {
    if (!b || !b->buffer) return NULL;
    return b->buffer->data;
}
