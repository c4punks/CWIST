#include "cwist/core/template/template.h"
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

/**
 * @file template.c
 * @brief Minimal template renderer supporting variable substitution and simple control flow.
 */

/** @brief Original source and shared diagnostic for the current file/string. */
typedef struct {
    const char *text;
    const char *file_path;
    const char *base_dir; /**< Directory used to resolve relative includes; may be NULL. */
    cwist_template_error_t *error;
} template_source;

/** @brief Command named by a {% ... %} tag. */
typedef enum {
    TAG_NONE,
    TAG_IF,
    TAG_ELSE,
    TAG_ENDIF,
    TAG_FOR,
    TAG_ENDFOR,
    TAG_INCLUDE
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
        {"if", TAG_IF},   {"else", TAG_ELSE},     {"endif", TAG_ENDIF},
        {"for", TAG_FOR}, {"endfor", TAG_ENDFOR}, {"include", TAG_INCLUDE},
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
                default: return "control";
            }
        default: return NULL;
    }
}

static void report_error(const template_source *source, const char *position,
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
    const template_tag_kind close_kind = open_kind == TAG_IF ? TAG_ENDIF : TAG_ENDFOR;
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
    report_error(source, opening, CWIST_TEMPLATE_ERROR_UNCLOSED_BLOCK);
    return NULL;
unclosed_tag:
    report_error(source, scan, CWIST_TEMPLATE_ERROR_UNCLOSED_TAG);
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
        case cJSON_Number: snprintf(buf, buf_len, "%g", value->valuedouble); return buf;
        case cJSON_True: return "true";
        case cJSON_False: return "false";
        default: return NULL;
    }
}

/**
 * @brief HTML-escape a string into an output buffer.
 */
static void html_escape(const char *value, char *out, size_t out_len) {
    if (!value || out_len == 0) return;
    size_t i, j;
    for (i = 0, j = 0; value[i] && j < out_len - 1; i++) {
        const char *repl = NULL;
        switch (value[i]) {
            case '&': repl = "&amp;"; break;
            case '<': repl = "&lt;"; break;
            case '>': repl = "&gt;"; break;
            case '"': repl = "&quot;"; break;
            case '\'': repl = "&#39;"; break;
            default: break;
        }
        if (repl) {
            size_t rlen = strlen(repl);
            if (j + rlen >= out_len) break;
            memcpy(out + j, repl, rlen);
            j += rlen;
        } else {
            out[j++] = value[i];
        }
    }
    out[j] = '\0';
}

/**
 * @brief Apply a simple filter to a string value in-place into a fixed buffer.
 * Supported filters: upper, lower, escape, trim, length, default(arg).
 * @param value Input string.
 * @param filter Filter name (may include an argument, e.g. "default(n/a)").
 * @param out Output buffer.
 * @param out_len Output buffer size.
 */
static void apply_filter(const char *value, const char *filter, char *out, size_t out_len) {
    if (!filter || out_len == 0) return;
    if (strcmp(filter, "upper") == 0) {
        size_t i;
        for (i = 0; i < out_len - 1 && value && value[i]; i++)
            out[i] = (char)toupper((unsigned char)value[i]);
        out[i] = '\0';
    } else if (strcmp(filter, "lower") == 0) {
        size_t i;
        for (i = 0; i < out_len - 1 && value && value[i]; i++)
            out[i] = (char)tolower((unsigned char)value[i]);
        out[i] = '\0';
    } else if (strcmp(filter, "escape") == 0 || strcmp(filter, "e") == 0) {
        html_escape(value, out, out_len);
    } else if (strcmp(filter, "trim") == 0) {
        if (!value || value[0] == '\0') {
            out[0] = '\0';
            return;
        }
        const char *s = value;
        while (isspace((unsigned char)*s)) s++;
        /* Guard against all-whitespace input: s may have advanced past the
         * last character, making e = value + vlen - 1 < s. */
        size_t vlen = strlen(value);
        const char *e = value + vlen - 1;
        while (e > s && isspace((unsigned char)*e)) e--;
        size_t len = (s > e) ? 0 : (size_t)(e - s + 1);
        if (len >= out_len) len = out_len - 1;
        memcpy(out, s, len);
        out[len] = '\0';
    } else if (strcmp(filter, "length") == 0 || strcmp(filter, "len") == 0) {
        snprintf(out, out_len, "%zu", value ? strlen(value) : 0);
    } else if (strncmp(filter, "default", 7) == 0) {
        const char *arg = NULL;
        const char *lp = strchr(filter, '(');
        const char *rp = strrchr(filter, ')');
        if (lp && rp && rp > lp) {
            arg = lp + 1;
        }
        if (value && value[0]) {
            snprintf(out, out_len, "%s", value);
        } else if (arg) {
            size_t arg_len = (size_t)(rp - arg);
            if (arg_len >= out_len) arg_len = out_len - 1;
            memcpy(out, arg, arg_len);
            out[arg_len] = '\0';
        } else {
            out[0] = '\0';
        }
    } else {
        if (value) {
            snprintf(out, out_len, "%s", value);
        } else {
            out[0] = '\0';
        }
    }
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

/** @brief Render @p block one level deeper and append it to @p output. */
static bool append_block(cwist_sstring *output, const char *block, const cJSON *context, int depth,
                         const template_source *source) {
    cwist_sstring *rendered = render_internal(&block, context, depth, source);
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
        report_error(source, NULL, CWIST_TEMPLATE_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    cwist_sstring *output = cwist_sstring_create();
    if (!output) {
        report_error(source, *template_str, CWIST_TEMPLATE_ERROR_NO_MEMORY);
        return NULL;
    }
    const char *p = *template_str;
    const char *start = p;

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
                if (*p) p += 2;
                start = p;
                break;

            case '{': { /* Variable: {{ key [| filter] }} */
                cwist_sstring_append_len(output, start, p - start);
                p += 2;
                const char *var_start = p;
                while (*p && (p[0] != '}' || p[1] != '}')) p++;
                if (!*p) {
                    report_error(source, opening, CWIST_TEMPLATE_ERROR_UNCLOSED_TAG);
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
                if (raw && filter) {
                    char filtered[512] = {0};
                    apply_filter(raw, filter, filtered, sizeof(filtered));
                    cwist_sstring_append(output, filtered);
                } else if (raw) {
                    cwist_sstring_append(output, raw);
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
                    report_error(source, opening, CWIST_TEMPLATE_ERROR_UNCLOSED_TAG);
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
                                report_error(source, opening, CWIST_TEMPLATE_ERROR_DEPTH);
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
                                report_error(source, opening, CWIST_TEMPLATE_ERROR_DEPTH);
                                goto fail;
                            }
                            cJSON *item;
                            cJSON_ArrayForEach(item, array) {
                                cJSON *loop_context = cJSON_Duplicate(context, 1);
                                cJSON *loop_item = cJSON_Duplicate(item, 1);
                                if (!loop_context || !loop_item ||
                                    !cJSON_AddItemToObject(loop_context, item_name, loop_item)) {
                                    report_error(source, opening, CWIST_TEMPLATE_ERROR_NO_MEMORY);
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

                    case TAG_ELSE:
                    case TAG_ENDIF:
                    case TAG_ENDFOR: *template_str = p; return output;

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
    const template_source source = {template_str, NULL, NULL, error};
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
static cwist_sstring *render_file_internal(const char *file_path, const cJSON *context, int depth,
                                           const template_source *parent, const char *include_tag) {
    if (!file_path || depth > CWIST_TEMPLATE_MAX_DEPTH) {
        report_error(parent, include_tag,
                     file_path ? CWIST_TEMPLATE_ERROR_DEPTH
                               : CWIST_TEMPLATE_ERROR_INVALID_ARGUMENT);
        return NULL;
    }
    FILE *f = fopen(file_path, "rb");
    if (!f) {
        report_error(parent, include_tag, CWIST_TEMPLATE_ERROR_IO);
        perror("Failed to open template file");
        return NULL;
    }

    cwist_template_error_code_t code = CWIST_TEMPLATE_ERROR_IO;
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

    const template_source source = {template_str, file_path, dir_buf, parent->error};
    const char *p = template_str;
    cwist_sstring *rendered = render_internal(&p, context, depth, &source);
    cwist_free(template_str);
    return rendered;

fail_close:
    fclose(f);
fail:
    report_error(parent, include_tag, code);
    cwist_free(template_str);
    return NULL;
}

cwist_sstring *cwist_template_render_file(const char *file_path, const cJSON *context) {
    return cwist_template_render_file_ex(file_path, context, NULL);
}

cwist_sstring *cwist_template_render_file_ex(const char *file_path, const cJSON *context,
                                             cwist_template_error_t *error) {
    if (error) memset(error, 0, sizeof(*error));
    const template_source source = {NULL, file_path, NULL, error};
    return render_file_internal(file_path, context, 0, &source, NULL);
}
