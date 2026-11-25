#include "cwist/core/template/template.h"
#include "../../sys/file_confine.h"
#include <cwist/core/mem/alloc.h>
#include <cwist/sys/platform.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include "../utils/number_internal.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#if defined(__GNUC__) || defined(__clang__)
#define TEMPLATE_NOINLINE __attribute__((noinline))
#else
#define TEMPLATE_NOINLINE
#endif

/**
 * @file template.c
 * @brief Minimal template renderer supporting variable substitution and simple control flow.
 */

typedef struct template_block_entry {
    char *name;
    char *content;
    struct template_block_entry *next;
} template_block_entry;

/** @brief Original source and shared diagnostic for the current file/string. */
typedef struct {
    const char *text;
    const char *file_path;
    const char *base_dir; /**< Directory used to resolve relative includes; may be NULL. */
    const char *include_root; /**< Canonical directory includes must stay in; NULL = any. */
    cwist_template_error_t *error;
    const template_block_entry *blocks;
    bool extends_allowed;
} template_source;

/** @brief Command named by a {% ... %} tag. */
typedef enum {
    TAG_NONE,
    TAG_IF,
    TAG_ELSE,
    TAG_ENDIF,
    TAG_FOR,
    TAG_ENDFOR,
    TAG_INCLUDE,
    TAG_EXTENDS,
    TAG_BLOCK,
    TAG_ENDBLOCK
} template_tag_kind;

/** @brief Forward declarations for recursive block and file rendering. */
static cwist_sstring *render_internal(const char **template_str, const cJSON *context, int depth,
                                      const template_source *source);
static cwist_sstring *render_file_internal(const char *file_path, const cJSON *context, int depth,
                                           const template_source *parent, const char *include_tag);

/** @brief Maximum nesting depth for includes/control structures. */
#define CWIST_TEMPLATE_MAX_DEPTH 32

/** @brief Separators between the words of a {% ... %} tag. */
#define TAG_DELIMS " \t\n"

/**
 * @brief Extract the directory component of a file path into a caller-supplied buffer.
 *
 * The result always ends without a trailing separator and is never empty: a
 * path with no directory component (e.g. "file.html") produces ".".  The
 * buffer is always NUL-terminated even when the path is longer than buf_len.
 *
 * @param path   Source file path.
 * @param buf    Output buffer to write the directory into.
 * @param buf_len Size of buf, including the NUL terminator slot.
 */
static void path_dirname(const char *path, char *buf, size_t buf_len) {
    if (!path || buf_len == 0) return;

    const char *last = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') last = p;
    }

    if (!last) {
        /* No separator found - directory is the current directory. */
        if (buf_len >= 2) {
            buf[0] = '.';
            buf[1] = '\0';
        } else {
            buf[0] = '\0';
        }
        return;
    }

    size_t len = (size_t)(last - path);
    if (len == 0) {
        /* Path starts with a separator, e.g. "/file.html". */
        len = 1;
    }
    if (len >= buf_len) len = buf_len - 1;
    memcpy(buf, path, len);
    buf[len] = '\0';
}

/**
 * @brief Resolve an include path against a base directory.
 *
 * Absolute paths (starting with '/' or a drive letter on Windows) are
 * returned unchanged.  Relative paths are prefixed with base_dir.  The
 * resulting path is written into buf and is always NUL-terminated.
 *
 * @param base_dir Directory of the including file (may be NULL or empty).
 * @param include_path The path token from the {% include "..." %} tag.
 * @param buf    Output buffer for the resolved path.
 * @param buf_len Size of buf.
 */
static void resolve_include_path(const char *base_dir, const char *include_path, char *buf,
                                 size_t buf_len) {
    if (!include_path || buf_len == 0) {
        if (buf_len > 0) buf[0] = '\0';
        return;
    }

    /* Absolute path: use as-is. */
    if (include_path[0] == '/' || include_path[0] == '\\' ||
        (include_path[0] != '\0' && include_path[1] == ':')) {
        snprintf(buf, buf_len, "%s", include_path);
        return;
    }

    /* Relative path: prepend base_dir when available. */
    if (base_dir && base_dir[0] != '\0' && !(base_dir[0] == '.' && base_dir[1] == '\0')) {
        snprintf(buf, buf_len, "%s/%s", base_dir, include_path);
    } else {
        snprintf(buf, buf_len, "%s", include_path);
    }
}

static template_tag_kind classify_command(const char *cmd, size_t len) {
    static const struct {
        const char *name;
        template_tag_kind kind;
    } commands[] = {
        {"if", TAG_IF},           {"else", TAG_ELSE},     {"endif", TAG_ENDIF},
        {"for", TAG_FOR},         {"endfor", TAG_ENDFOR}, {"include", TAG_INCLUDE},
        {"extends", TAG_EXTENDS}, {"block", TAG_BLOCK},   {"endblock", TAG_ENDBLOCK},
    };
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (strlen(commands[i].name) == len && strncmp(cmd, commands[i].name, len) == 0)
            return commands[i].kind;
    }
    return TAG_NONE;
}

/** @brief Classify the command of a tag body (the text right after "{%"). */
static template_tag_kind tag_command(const char *body) {
    while (isspace((unsigned char)*body)) body++;
    return classify_command(body, strcspn(body, " \t\r\n%}"));
}

static const char *tag_type(const char *tag) {
    if (!tag || tag[0] != '{') return NULL;
    switch (tag[1]) {
        case '{': return "variable";
        case '%':
            switch (tag_command(tag + 2)) {
                case TAG_IF: return "if";
                case TAG_FOR: return "for";
                case TAG_INCLUDE: return "include";
                case TAG_EXTENDS: return "extends";
                case TAG_BLOCK: return "block";
                default: return "control";
            }
        default: return NULL;
    }
}

static void template_report_error(const template_source *source, const char *position,
                                  cwist_template_error_code_t code) {
    cwist_template_error_t *error = source->error;
    if (!error || error->code != CWIST_TEMPLATE_ERROR_NONE) return;
    error->code = code;
    error->byte_offset = source->text && position ? (size_t)(position - source->text) : 0;
    error->tag_type = tag_type(position);
    snprintf(error->file_path, sizeof(error->file_path), "%s",
             source->file_path ? source->file_path : "");
}

/** @brief Locate a matching control terminator without reading tags inside comments/variables. */
static const char *find_block_end(const char *start, const char *opening,
                                  template_tag_kind open_kind, const char **else_pos,
                                  const template_source *source) {
    const template_tag_kind close_kind =
        open_kind == TAG_IF ? TAG_ENDIF : (open_kind == TAG_FOR ? TAG_ENDFOR : TAG_ENDBLOCK);
    int nested = 0;
    const char *scan = start;
    const char *end;
    while (*scan) {
        if (scan[0] != '{') {
            scan++;
            continue;
        }
        switch (scan[1]) {
            case '#':
                end = strstr(scan + 2, "#}");
                if (!end) goto unclosed_block;
                scan = end + 2;
                break;
            case '{':
                end = strstr(scan + 2, "}}");
                if (!end) goto unclosed_tag;
                scan = end + 2;
                break;
            case '%': {
                end = strstr(scan + 2, "%}");
                if (!end) goto unclosed_tag;
                template_tag_kind kind = tag_command(scan + 2);
                if (kind == open_kind) {
                    nested++;
                } else if (kind == close_kind) {
                    if (nested == 0) return scan;
                    nested--;
                } else if (kind == TAG_ELSE && else_pos && !*else_pos && nested == 0) {
                    *else_pos = scan;
                }
                scan = end + 2;
                break;
            }
            default: scan++; break;
        }
    }

unclosed_block:
    template_report_error(source, opening, CWIST_TEMPLATE_ERROR_UNCLOSED_BLOCK);
    return NULL;
unclosed_tag:
    template_report_error(source, scan, CWIST_TEMPLATE_ERROR_UNCLOSED_TAG);
    return NULL;
}

/** @brief Truthiness used by {% if %}: non-empty strings/containers, non-zero numbers, true. */
static bool json_truthy(const cJSON *value) {
    if (!value) return false;
    switch (value->type & 0xFF) {
        case cJSON_True: return true;
        case cJSON_String: return value->valuestring && value->valuestring[0] != '\0';
        case cJSON_Object:
        case cJSON_Array: return cJSON_GetArraySize(value) > 0;
        case cJSON_Number: return value->valuedouble != 0;
        default: return false;
    }
}

/** @brief Text form of a scalar value for {{ }} output, or NULL when it prints nothing. */
static const char *json_scalar_text(const cJSON *value, char *buf, size_t buf_len) {
    switch (value->type & 0xFF) {
        case cJSON_String: return value->valuestring;
        case cJSON_Number: cwist_number_format_double(buf, buf_len, value->valuedouble); return buf;
        case cJSON_True: return "true";
        case cJSON_False: return "false";
        default: return NULL;
    }
}

/**
 * @brief Apply a simple filter and return a dynamically sized string.
 * Supported filters: upper, lower, escape, trim, length, default(arg).
 */
static cwist_sstring *apply_filter(const char *value, const char *filter) {
    if (!filter) return NULL;
    cwist_sstring *out = cwist_sstring_create();
    if (!out) return NULL;

    if (strcmp(filter, "upper") == 0 || strcmp(filter, "lower") == 0) {
        bool upper = strcmp(filter, "upper") == 0;
        if (value) {
            const char *p = value;
            while (*p) {
                char chunk[256];
                size_t len = strlen(p) < sizeof(chunk) ? strlen(p) : sizeof(chunk);
                for (size_t i = 0; i < len; i++) {
                    unsigned char c = (unsigned char)p[i];
                    chunk[i] = (char)(upper ? toupper(c) : tolower(c));
                }
                cwist_sstring_append_len(out, chunk, len);
                p += len;
            }
        }
    } else if (strcmp(filter, "escape") == 0 || strcmp(filter, "e") == 0) {
        cwist_sstring_append_escaped(out, value);
    } else if (strcmp(filter, "trim") == 0) {
        if (value && *value) {
            const char *s = value;
            while (isspace((unsigned char)*s)) s++;
            size_t vlen = strlen(value);
            const char *e = value + vlen - 1;
            while (e > s && isspace((unsigned char)*e)) e--;
            if (s <= e) cwist_sstring_append_len(out, s, (size_t)(e - s + 1));
        }
    } else if (strcmp(filter, "length") == 0 || strcmp(filter, "len") == 0) {
        /* Code points, not bytes: UTF-8 continuation bytes are not counted
         * (#986). Arrays and objects are counted in apply_filters(). */
        size_t count = 0;
        for (const unsigned char *c = (const unsigned char *)(value ? value : ""); *c; c++)
            if ((*c & 0xC0) != 0x80) count++;
        char length[64];
        snprintf(length, sizeof(length), "%zu", count);
        cwist_sstring_append(out, length);
    } else if (value) {
        cwist_sstring_append(out, value);
    }
    return out;
}

/**
 * @brief Split the next filter off a chain at a '|' outside quotes.
 * @return The filter (trimmed, NUL-terminated in place), or NULL at the end.
 */
static char *next_filter(char **cursor) {
    char *p = *cursor;
    if (!p) return NULL;
    while (isspace((unsigned char)*p) || *p == '|') p++;
    if (!*p) {
        *cursor = NULL;
        return NULL;
    }
    char *start = p;
    char quote = 0;
    for (; *p; p++) {
        if (quote) {
            if (*p == quote) quote = 0;
        } else if (*p == '"' || *p == '\'') {
            quote = *p;
        } else if (*p == '|') {
            break;
        }
    }
    *cursor = *p ? p + 1 : NULL;
    *p = '\0';
    char *end = p;
    while (end > start && isspace((unsigned char)end[-1])) *--end = '\0';
    return start;
}

/**
 * @brief Argument of default(...): a quoted string literal (quotes removed)
 *        or, for compatibility, the bare text between the parentheses.
 * @return true when @p filter is a well-formed default(...) call.
 */
static bool default_argument(const char *filter, const char **arg, size_t *arg_len) {
    if (strncmp(filter, "default", 7) != 0) return false;
    const char *lp = filter + 7;
    while (isspace((unsigned char)*lp)) lp++;
    if (*lp != '(') return false;
    const char *rp = strrchr(lp, ')');
    if (!rp) return false;
    const char *s = lp + 1;
    const char *e = rp;
    while (s < e && isspace((unsigned char)*s)) s++;
    while (e > s && isspace((unsigned char)e[-1])) e--;
    if (e - s >= 2 && (*s == '"' || *s == '\'') && e[-1] == *s) {
        s++;
        e--;
    }
    *arg = s;
    *arg_len = (size_t)(e - s);
    return true;
}

/**
 * @brief Apply a filter chain to a variable.
 * @param value Scalar text of the variable, or NULL when it is missing, null,
 *              an array or an object.
 * @param node  The variable's JSON node (NULL when missing); lets length
 *              count array and object members.
 * @param filters Filter chain after the first '|' (modified in place), or NULL.
 * @return Rendered text, or NULL when there is nothing to print.
 */
static cwist_sstring *apply_filters(const char *value, const cJSON *node, char *filters) {
    bool have = value != NULL;
    cwist_sstring *current = cwist_sstring_create();
    if (!current) return NULL;
    cwist_sstring_append(current, value ? value : "");

    bool autoescape = true;
    bool escaped = false;
    bool first = true;
    char *cursor = filters;
    char *filter;
    while ((filter = next_filter(&cursor)) != NULL) {
        const char *arg = NULL;
        size_t arg_len = 0;
        if (strcmp(filter, "safe") == 0 || strcmp(filter, "raw") == 0) {
            autoescape = false;
        } else if (default_argument(filter, &arg, &arg_len)) {
            /* Applies to a missing variable as well as an empty one (#985). */
            if (!have || !current->data || !current->data[0]) {
                cwist_sstring_destroy(current);
                current = cwist_sstring_create();
                if (!current) return NULL;
                cwist_sstring_assign(current, "");
                cwist_sstring_append_len(current, arg, arg_len);
                have = true;
            }
        } else if (first && (strcmp(filter, "length") == 0 || strcmp(filter, "len") == 0) &&
                   (cJSON_IsArray(node) || cJSON_IsObject(node) || !have)) {
            /* Item count for arrays and objects, 0 for a missing value (#986). */
            char length[64];
            snprintf(length, sizeof(length), "%d", node ? cJSON_GetArraySize(node) : 0);
            cwist_sstring_destroy(current);
            current = cwist_sstring_create();
            if (!current) return NULL;
            cwist_sstring_assign(current, length);
            have = true;
        } else if (have) {
            cwist_sstring *next = apply_filter(current->data, filter);
            if (!next) {
                cwist_sstring_destroy(current);
                return NULL;
            }
            cwist_sstring_destroy(current);
            current = next;
            /* Sticky: once escaped, later filters (upper, trim, ...) work
             * on markup, and escaping it again would print "&amp;lt;". */
            if (strcmp(filter, "escape") == 0 || strcmp(filter, "e") == 0) escaped = true;
        }
        first = false;
    }
    if (!have) {
        cwist_sstring_destroy(current);
        return NULL;
    }
    if (autoescape && !escaped) {
        cwist_sstring *next = cwist_sstring_create();
        if (!next) {
            cwist_sstring_destroy(current);
            return NULL;
        }
        cwist_sstring_append_escaped(next, current->data);
        cwist_sstring_destroy(current);
        current = next;
    }
    return current;
}

/**
 * @brief Resolve a value from the current JSON context using dotted lookup syntax.
 * @param context Current object/array context used for template expansion.
 * @param key Dot-separated key path, or "." for the current loop item.
 * @return Matching cJSON node, or NULL when the path cannot be resolved.
 */
static const cJSON *get_value_from_context(const cJSON *context, const char *key) {
    if (!context || !key) return NULL;

    // Handle the special case of "." referring to the current context in a loop
    if (strcmp(key, ".") == 0) {
        return context;
    }

    char *key_copy = cwist_strdup(key);
    if (!key_copy) return NULL;
    char *ptr_to_free = key_copy;
    char *saveptr = NULL;
    char *token = strtok_r(key_copy, ".", &saveptr);
    const cJSON *current = context;

    while (token != NULL) {
        if (!cJSON_IsObject(current)) {
            current = NULL;
            break;
        }
        current = cJSON_GetObjectItem(current, token);
        if (!current) break;
        token = strtok_r(NULL, ".", &saveptr);
    }

    cwist_free(ptr_to_free);
    return current;
}

static const template_block_entry *find_block_override(const template_block_entry *blocks,
                                                       const char *name) {
    for (const template_block_entry *b = blocks; b; b = b->next) {
        if (strcmp(b->name, name) == 0) return b;
    }
    return NULL;
}

static void free_block_list(template_block_entry *head) {
    while (head) {
        template_block_entry *next = head->next;
        cwist_free(head->name);
        cwist_free(head->content);
        cwist_free(head);
        head = next;
    }
}

static template_block_entry *extract_child_blocks(const char *template_str,
                                                  const template_source *source) {
    template_block_entry *head = NULL;
    const char *p = template_str;
    while (*p) {
        if (p[0] != '{') {
            p++;
            continue;
        }
        if (p[1] == '#') {
            const char *end = strstr(p + 2, "#}");
            if (!end) break;
            p = end + 2;
            continue;
        }
        if (p[1] == '{') {
            const char *end = strstr(p + 2, "}}");
            if (!end) break;
            p = end + 2;
            continue;
        }
        if (p[1] == '%') {
            const char *opening = p;
            const char *tag_end = strstr(p + 2, "%}");
            if (!tag_end) break;
            char tag[256] = {0};
            size_t tag_len = (size_t)(tag_end - (p + 2));
            if (tag_len >= sizeof(tag)) tag_len = sizeof(tag) - 1;
            memcpy(tag, p + 2, tag_len);
            p = tag_end + 2;

            char *tag_saveptr = NULL;
            char *cmd = strtok_r(tag, TAG_DELIMS, &tag_saveptr);
            if (cmd && classify_command(cmd, strlen(cmd)) == TAG_BLOCK) {
                char *bname = strtok_r(NULL, TAG_DELIMS, &tag_saveptr);
                if (bname) {
                    const char *block_body_start = p;
                    const char *block_end = find_block_end(p, opening, TAG_BLOCK, NULL, source);
                    if (!block_end) {
                        free_block_list(head);
                        return NULL;
                    }
                    size_t blen = (size_t)(block_end - block_body_start);
                    template_block_entry *entry =
                        (template_block_entry *)cwist_alloc(sizeof(*entry));
                    if (!entry) {
                        free_block_list(head);
                        return NULL;
                    }
                    memset(entry, 0, sizeof(*entry));
                    entry->name = cwist_strdup(bname);
                    entry->content = (char *)cwist_alloc(blen + 1);
                    if (!entry->name || !entry->content) {
                        cwist_free(entry->name);
                        cwist_free(entry->content);
                        cwist_free(entry);
                        free_block_list(head);
                        return NULL;
                    }
                    memcpy(entry->content, block_body_start, blen);
                    entry->content[blen] = '\0';
                    entry->next = head;
                    head = entry;

                    p = strstr(block_end, "%}") + 2;
                }
            }
        } else {
            p++;
        }
    }
    return head;
}

/** @brief Render @p block one level deeper and append it to @p output. */
static bool append_block(cwist_sstring *output, const char *block, const cJSON *context, int depth,
                         const template_source *source) {
    template_source block_source = *source;
    block_source.extends_allowed = false;
    cwist_sstring *rendered = render_internal(&block, context, depth, &block_source);
    if (!rendered) return false;
    cwist_sstring_append(output, rendered->data);
    cwist_sstring_destroy(rendered);
    return true;
}

/**
 * @brief Recursively render a template string until the current control block ends.
 * @param template_str Cursor into the template source; advanced as tags are consumed.
 * @param context JSON context used for variable lookups and loop bindings.
 * @return Newly allocated rendered string fragment, or NULL on invalid input.
 */
static cwist_sstring *render_internal(const char **template_str, const cJSON *context, int depth,
                                      const template_source *source) {
    if (!template_str || !*template_str) {
        template_report_error(source, NULL, CWIST_TEMPLATE_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    cwist_sstring *output = cwist_sstring_create();
    /* Start from "" so an empty render still returns a NUL-terminated
     * buffer instead of data == NULL (#984). */
    if (output) {
        cwist_error_t init_err = cwist_sstring_assign(output, "");
        bool init_ok = cwist_error_is_ok(&init_err);
        cwist_error_dispose(&init_err);
        if (!init_ok || !output->data) {
            cwist_sstring_destroy(output);
            output = NULL;
        }
    }
    if (!output) {
        template_report_error(source, *template_str, CWIST_TEMPLATE_ERROR_NO_MEMORY);
        return NULL;
    }
    const char *p = *template_str;
    const char *start = p;

    /* Check for {% extends "parent.html" %} at the start of a template file. */
    if (source->extends_allowed) {
        const char *scan = p;
        while (isspace((unsigned char)*scan)) scan++;
        if (scan[0] == '{' && scan[1] == '%') {
            const char *tag_end = strstr(scan + 2, "%}");
            if (tag_end) {
                char first_tag[256] = {0};
                size_t ftlen = (size_t)(tag_end - (scan + 2));
                if (ftlen >= sizeof(first_tag)) ftlen = sizeof(first_tag) - 1;
                memcpy(first_tag, scan + 2, ftlen);
                char *sp = NULL;
                char *cmd = strtok_r(first_tag, TAG_DELIMS, &sp);
                if (cmd && classify_command(cmd, strlen(cmd)) == TAG_EXTENDS) {
                    char *parent_path = strtok_r(NULL, TAG_DELIMS, &sp);
                    if (parent_path) {
                        while (*parent_path == ' ' || *parent_path == '\t' || *parent_path == '\n')
                            parent_path++;
                        char *end = parent_path + strlen(parent_path);
                        while (end > parent_path &&
                               (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' ||
                                end[-1] == '"' || end[-1] == '\''))
                            *--end = '\0';
                        if (parent_path[0] == '"' || parent_path[0] == '\'') parent_path++;

                        char resolved[4096];
                        resolve_include_path(source->base_dir, parent_path, resolved,
                                             sizeof(resolved));

                        template_block_entry *child_blocks =
                            extract_child_blocks(tag_end + 2, source);
                        template_block_entry *inherited_tail =
                            (template_block_entry *)source->blocks;
                        while (inherited_tail && inherited_tail->next)
                            inherited_tail = inherited_tail->next;
                        template_source parent_source = *source;
                        parent_source.blocks = source->blocks ? source->blocks : child_blocks;
                        parent_source.extends_allowed = true;
                        if (inherited_tail) inherited_tail->next = child_blocks;

                        cwist_sstring *res = render_file_internal(resolved, context, depth + 1,
                                                                  &parent_source, scan);
                        if (inherited_tail) inherited_tail->next = NULL;
                        free_block_list(child_blocks);
                        cwist_sstring_destroy(output);
                        if (!res) return NULL;
                        *template_str = p + strlen(p);
                        return res;
                    }
                }
            }
        }
    }

    while (*p) {
        if (p[0] != '{') {
            p++;
            continue;
        }
        const char *opening = p;

        switch (p[1]) {
            case '#': /* Comment: {# ... #} -- skip silently. */
                cwist_sstring_append_len(output, start, p - start);
                p += 2;
                while (*p && !(p[0] == '#' && p[1] == '}')) p++;
                if (!*p) {
                    template_report_error(source, opening, CWIST_TEMPLATE_ERROR_UNCLOSED_TAG);
                    goto fail;
                }
                p += 2;
                start = p;
                break;

            case '{': { /* Variable: {{ key [| filter] }} */
                cwist_sstring_append_len(output, start, p - start);
                p += 2;
                const char *var_start = p;
                while (*p && (p[0] != '}' || p[1] != '}')) p++;
                if (!*p) {
                    template_report_error(source, opening, CWIST_TEMPLATE_ERROR_UNCLOSED_TAG);
                    goto fail;
                }

                char var_buf[256] = {0};
                size_t var_len = (size_t)(p - var_start);
                if (var_len >= sizeof(var_buf)) var_len = sizeof(var_buf) - 1;
                memcpy(var_buf, var_start, var_len);

                char *trimmed_var = var_buf;
                while (*trimmed_var == ' ') trimmed_var++;
                char *end = trimmed_var + strlen(trimmed_var);
                while (end > trimmed_var && end[-1] == ' ') *--end = '\0';

                char *filter = NULL;
                char *pipe = strstr(trimmed_var, "|");
                if (pipe) {
                    char *ve = pipe;
                    while (ve > trimmed_var && isspace((unsigned char)ve[-1])) *--ve = '\0';
                    *pipe = '\0';
                    filter = pipe + 1;
                    while (*filter == ' ') filter++;
                    char *fe = filter + strlen(filter);
                    while (fe > filter && fe[-1] == ' ') *--fe = '\0';
                }

                const cJSON *value = get_value_from_context(context, trimmed_var);
                char num_str[64] = {0};
                const char *raw = value ? json_scalar_text(value, num_str, sizeof(num_str)) : NULL;
                if (raw || filter) {
                    cwist_sstring *filtered = apply_filters(raw, value, filter);
                    if (filtered) {
                        cwist_sstring_append(output, filtered->data);
                        cwist_sstring_destroy(filtered);
                    }
                }
                p += 2;
                start = p;
                break;
            }

            case '%': { /* Tag: {% ... %} */
                cwist_sstring_append_len(output, start, p - start);
                p += 2;
                const char *tag_start = p;
                while (*p && (p[0] != '%' || p[1] != '}')) p++;
                if (!*p) {
                    template_report_error(source, opening, CWIST_TEMPLATE_ERROR_UNCLOSED_TAG);
                    goto fail;
                }

                char tag[256] = {0};
                size_t tag_len = (size_t)(p - tag_start);
                if (tag_len >= sizeof(tag)) tag_len = sizeof(tag) - 1;
                memcpy(tag, tag_start, tag_len);
                p += 2; /* past "%}": start of a block body, or of the text after the tag */

                char *tag_saveptr = NULL;
                char *cmd = strtok_r(tag, TAG_DELIMS, &tag_saveptr);
                template_tag_kind kind = cmd ? classify_command(cmd, strlen(cmd)) : TAG_NONE;

                switch (kind) {
                    case TAG_IF: {
                        char *tok = strtok_r(NULL, TAG_DELIMS, &tag_saveptr);
                        bool negate = tok && strcmp(tok, "not") == 0;
                        if (negate) tok = strtok_r(NULL, TAG_DELIMS, &tag_saveptr);
                        bool cond = json_truthy(tok ? get_value_from_context(context, tok) : NULL);
                        if (negate) cond = !cond;

                        const char *else_pos = NULL;
                        const char *block_end =
                            find_block_end(p, opening, TAG_IF, &else_pos, source);
                        if (!block_end) goto fail;

                        const char *branch = NULL;
                        if (cond)
                            branch = p;
                        else if (else_pos)
                            branch = strstr(else_pos, "%}") + 2;
                        if (branch) {
                            if (depth >= CWIST_TEMPLATE_MAX_DEPTH) {
                                template_report_error(source, opening, CWIST_TEMPLATE_ERROR_DEPTH);
                                goto fail;
                            }
                            if (!append_block(output, branch, context, depth + 1, source))
                                goto fail;
                        }
                        p = strstr(block_end, "%}") + 2;
                        break;
                    }

                    case TAG_FOR: {
                        char *item_name = strtok_r(NULL, TAG_DELIMS, &tag_saveptr);
                        strtok_r(NULL, TAG_DELIMS, &tag_saveptr); /* "in" */
                        char *array_name = strtok_r(NULL, TAG_DELIMS, &tag_saveptr);
                        const cJSON *array = (array_name && context)
                                                 ? get_value_from_context(context, array_name)
                                                 : NULL;

                        const char *block_end = find_block_end(p, opening, TAG_FOR, NULL, source);
                        if (!block_end) goto fail;

                        if (item_name && cJSON_IsArray(array)) {
                            if (cJSON_GetArraySize(array) > 0 &&
                                depth >= CWIST_TEMPLATE_MAX_DEPTH) {
                                template_report_error(source, opening, CWIST_TEMPLATE_ERROR_DEPTH);
                                goto fail;
                            }
                            cJSON *item;
                            cJSON_ArrayForEach(item, array) {
                                cJSON *loop_context = cJSON_Duplicate(context, 1);
                                cJSON *loop_item = cJSON_Duplicate(item, 1);
                                if (!loop_context || !loop_item ||
                                    !cJSON_AddItemToObject(loop_context, item_name, loop_item)) {
                                    template_report_error(source, opening,
                                                          CWIST_TEMPLATE_ERROR_NO_MEMORY);
                                    cJSON_Delete(loop_item);
                                    cJSON_Delete(loop_context);
                                    goto fail;
                                }
                                bool ok = append_block(output, p, loop_context, depth + 1, source);
                                cJSON_Delete(loop_context);
                                if (!ok) goto fail;
                            }
                        }
                        p = strstr(block_end, "%}") + 2;
                        break;
                    }

                    case TAG_INCLUDE: {
                        char *path = strtok_r(NULL, TAG_DELIMS, &tag_saveptr);
                        if (!path) break;
                        while (*path == ' ' || *path == '\t' || *path == '\n') path++;
                        char *end = path + strlen(path);
                        while (end > path && (end[-1] == ' ' || end[-1] == '\t' ||
                                              end[-1] == '\n' || end[-1] == '"'))
                            *--end = '\0';
                        if (path[0] == '"') path++;
                        if (!path[0]) break;

                        /* Resolve against the including file's directory so relative
                         * includes do not depend on the process working directory. */
                        char resolved[4096];
                        resolve_include_path(source->base_dir, path, resolved, sizeof(resolved));
                        cwist_sstring *included =
                            render_file_internal(resolved, context, depth + 1, source, opening);
                        if (!included) goto fail;
                        cwist_sstring_append(output, included->data);
                        cwist_sstring_destroy(included);
                        break;
                    }

                    case TAG_BLOCK: {
                        char *bname = strtok_r(NULL, TAG_DELIMS, &tag_saveptr);
                        const char *block_end = find_block_end(p, opening, TAG_BLOCK, NULL, source);
                        if (!block_end) goto fail;

                        const template_block_entry *override =
                            bname ? find_block_override(source->blocks, bname) : NULL;
                        if (override && override->content) {
                            if (depth >= CWIST_TEMPLATE_MAX_DEPTH) {
                                template_report_error(source, opening, CWIST_TEMPLATE_ERROR_DEPTH);
                                goto fail;
                            }
                            if (!append_block(output, override->content, context, depth + 1,
                                              source))
                                goto fail;
                        } else {
                            if (depth >= CWIST_TEMPLATE_MAX_DEPTH) {
                                template_report_error(source, opening, CWIST_TEMPLATE_ERROR_DEPTH);
                                goto fail;
                            }
                            if (!append_block(output, p, context, depth + 1, source)) goto fail;
                        }
                        p = strstr(block_end, "%}") + 2;
                        break;
                    }

                    case TAG_EXTENDS: break;

                    case TAG_ELSE:
                    case TAG_ENDIF:
                    case TAG_ENDFOR:
                    case TAG_ENDBLOCK: *template_str = p; return output;

                    case TAG_NONE: break;
                }
                start = p;
                break;
            }

            default: p++; break;
        }
    }

    cwist_sstring_append_len(output, start, p - start);
    *template_str = p;
    return output;

fail:
    cwist_sstring_destroy(output);
    return NULL;
}

/**
 * @brief Render an in-memory template string against a JSON context object.
 *
 * Include paths in an in-memory template have no directory origin, so relative
 * includes resolve against the process working directory. Use
 * cwist_template_render_file() when file-relative includes are needed.
 *
 * @param template_str Template source to render.
 * @param context JSON object supplying values for substitutions and control flow.
 * @return Heap-allocated rendered output, or NULL on failure.
 */
cwist_sstring *cwist_template_render(const char *template_str, const cJSON *context) {
    return cwist_template_render_ex(template_str, context, NULL);
}

cwist_sstring *cwist_template_render_ex(const char *template_str, const cJSON *context,
                                        cwist_template_error_t *error) {
    if (error) memset(error, 0, sizeof(*error));
    const template_source source = {template_str, NULL, NULL, NULL, error, NULL, true};
    const char *p = template_str;
    return render_internal(&p, context, 0, &source);
}

/**
 * @brief Canonicalize @p include_root into @p out, reporting a bad root.
 *
 * Bare-metal builds have no realpath(), so they cannot enforce a root and
 * refuse every one instead of rendering unconfined.
 */
static bool canonical_root(const char *include_root, char out[PATH_MAX],
                           cwist_template_error_t *error) {
#if defined(CWIST_BAREMETAL)
    (void)include_root;
    (void)out;
#else
    if (include_root && realpath(include_root, out)) return true;
#endif
    const template_source source = {NULL, NULL, NULL, NULL, error, NULL, false};
    template_report_error(&source, NULL, CWIST_TEMPLATE_ERROR_INVALID_ARGUMENT);
    return false;
}

cwist_sstring *cwist_template_render_rooted(const char *template_str, const cJSON *context,
                                            const char *include_root,
                                            cwist_template_error_t *error) {
    if (error) memset(error, 0, sizeof(*error));
    char root[PATH_MAX];
    if (!canonical_root(include_root, root, error)) return NULL;
    const template_source source = {template_str, NULL, root, root, error, NULL, true};
    const char *p = template_str;
    return render_internal(&p, context, 0, &source);
}

/**
 * @brief Load a template file from disk and render it against a JSON context.
 *
 * The directory of file_path is used as the base for resolving relative
 * {% include "..." %} paths inside the template, so partials can be placed
 * next to the template that includes them without depending on the process
 * working directory.
 *
 * @param file_path Path to the template file to open.
 * @param context JSON object supplying values for substitutions and control flow.
 * @return Heap-allocated rendered output, or NULL when file IO or rendering fails.
 */
/**
 * @brief Open an included file only if its canonical path lies inside @p root.
 *
 * Kept out of line so the PATH_MAX buffer is not part of every recursive
 * render frame. The canonical path is what gets opened, so the file checked
 * is the file read.
 */
static TEMPLATE_NOINLINE FILE *open_confined(const char *root, const char *path,
                                             cwist_template_error_code_t *code) {
#if defined(CWIST_BAREMETAL)
    /* No realpath() in newlib; canonical_root() already refused the root. */
    (void)root;
    (void)path;
    *code = CWIST_TEMPLATE_ERROR_INVALID_ARGUMENT;
    return NULL;
#else
    char canonical[PATH_MAX];
    if (!realpath(path, canonical)) {
        *code = CWIST_TEMPLATE_ERROR_IO;
        return NULL;
    }
    if (!cwist_file_path_is_inside(canonical, root)) {
        *code = CWIST_TEMPLATE_ERROR_FORBIDDEN;
        return NULL;
    }
    int fd = cwist_file_open_confined(root, canonical);
    if (fd < 0) {
        *code = CWIST_TEMPLATE_ERROR_FORBIDDEN;
        return NULL;
    }
    FILE *f = fdopen(fd, "rb");
    if (!f) {
        close(fd);
        *code = CWIST_TEMPLATE_ERROR_IO;
    }
    return f;
#endif
}

static cwist_sstring *render_file_internal(const char *file_path, const cJSON *context, int depth,
                                           const template_source *parent, const char *include_tag) {
    if (!file_path || depth > CWIST_TEMPLATE_MAX_DEPTH) {
        template_report_error(parent, include_tag,
                              file_path ? CWIST_TEMPLATE_ERROR_DEPTH
                                        : CWIST_TEMPLATE_ERROR_INVALID_ARGUMENT);
        return NULL;
    }
    cwist_template_error_code_t code = CWIST_TEMPLATE_ERROR_IO;
    FILE *f = include_tag && parent->include_root
                  ? open_confined(parent->include_root, file_path, &code)
                  : fopen(file_path, "rb");
    if (!f) {
        template_report_error(parent, include_tag, code);
        if (code == CWIST_TEMPLATE_ERROR_IO) perror("Failed to open template file");
        return NULL;
    }

    char *template_str = NULL;
    long len;
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0)
        goto fail_close;

    template_str = cwist_alloc((size_t)len + 1);
    if (!template_str) {
        code = CWIST_TEMPLATE_ERROR_NO_MEMORY;
        goto fail_close;
    }

    size_t read_len = fread(template_str, 1, (size_t)len, f);
    bool read_failed = read_len != (size_t)len || ferror(f);
    if (fclose(f) != 0 || read_failed) goto fail;
    template_str[read_len] = '\0';

    char dir_buf[4096];
    path_dirname(file_path, dir_buf, sizeof(dir_buf));

    const template_source source = {template_str,  file_path,      dir_buf, parent->include_root,
                                    parent->error, parent->blocks, true};
    const char *p = template_str;
    cwist_sstring *rendered = render_internal(&p, context, depth, &source);
    cwist_free(template_str);
    return rendered;

fail_close:
    fclose(f);
fail:
    template_report_error(parent, include_tag, code);
    cwist_free(template_str);
    return NULL;
}

cwist_sstring *cwist_template_render_file(const char *file_path, const cJSON *context) {
    return cwist_template_render_file_ex(file_path, context, NULL);
}

cwist_sstring *cwist_template_render_file_ex(const char *file_path, const cJSON *context,
                                             cwist_template_error_t *error) {
    if (error) memset(error, 0, sizeof(*error));
    const template_source source = {NULL, file_path, NULL, NULL, error, NULL, false};
    return render_file_internal(file_path, context, 0, &source, NULL);
}

cwist_sstring *cwist_template_render_file_rooted(const char *file_path, const cJSON *context,
                                                 const char *include_root,
                                                 cwist_template_error_t *error) {
    if (error) memset(error, 0, sizeof(*error));
    char root[PATH_MAX];
    if (!canonical_root(include_root, root, error)) return NULL;
    const template_source source = {NULL, file_path, NULL, root, error, NULL, false};
    return render_file_internal(file_path, context, 0, &source, NULL);
}
