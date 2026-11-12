/**
 * @file template.h
 * @brief Simple template engine.
 */

#ifndef CWIST_TEMPLATE_H
#define CWIST_TEMPLATE_H

#include <cwist/core/sstring/sstring.h>
#include <cjson/cJSON.h>
#include <stddef.h>

/** @brief Reason a template could not be rendered. */
typedef enum {
    CWIST_TEMPLATE_ERROR_NONE = 0,
    CWIST_TEMPLATE_ERROR_INVALID_ARGUMENT,
    CWIST_TEMPLATE_ERROR_UNCLOSED_TAG,
    CWIST_TEMPLATE_ERROR_UNCLOSED_BLOCK,
    CWIST_TEMPLATE_ERROR_DEPTH,
    CWIST_TEMPLATE_ERROR_IO,
    CWIST_TEMPLATE_ERROR_NO_MEMORY,
    CWIST_TEMPLATE_ERROR_FORBIDDEN /**< An include resolved outside the include root. */
} cwist_template_error_code_t;

/**
 * @brief Optional render diagnostic, owned entirely by the caller.
 *
 * byte_offset is zero-based within the source named by file_path (or the
 * input string when file_path is empty). Syntax errors point to the opening
 * delimiter of the offending tag. Include I/O and depth errors point to the
 * include tag in its parent source; a top-level file I/O error has offset 0.
 * An include rejected by the include root (CWIST_TEMPLATE_ERROR_FORBIDDEN)
 * also points to the include tag.
 * tag_type is a static string ("variable", "control", "if", "for", or
 * "include"), or NULL when no tag is involved. file_path is copied and
 * NUL-terminated; paths longer than its capacity are truncated.
 * No initialization or cleanup is required. Each diagnostic render call
 * resets every field, including on success.
 */
typedef struct {
    cwist_template_error_code_t code;
    size_t byte_offset;
    const char *tag_type;
    char file_path[4096];
} cwist_template_error_t;

/**
 * @brief Renders a template string with the given cJSON context.
 *
 * This function parses a template string and replaces placeholders with values
 * from the provided cJSON context.
 *
 * It supports:
 * - Variable replacement: {{ key }}
 * - Variable filters: {{ key | upper }}, {{ key | lower }}, {{ key | escape }},
 *                     {{ key | trim }}, {{ key | length }}, {{ key | default("n/a") }}
 * - Simple conditionals: {% if key %}...{% endif %}
 * - Negated conditionals: {% if not key %}...{% endif %}
 * - Else branches: {% if key %}...{% else %}...{% endif %}
 * - Looping over arrays: {% for item in array %}...{{ item.key }}...{% endfor %}
 * - File includes: {% include "partial.html" %}
 * - Comments: {# ignored #}
 *
 * @param template_str The template string to render.
 * @param context The cJSON object containing the data for the template.
 * @return A new cwist_sstring containing the rendered output. The caller is
 *         responsible for destroying the returned string. Returns NULL on failure.
 */
cwist_sstring *cwist_template_render(const char *template_str, const cJSON *context);

/**
 * @brief Renders a template from a file with the given cJSON context.
 *
 * Reads a template from the specified file path and renders it using the
 * provided cJSON context.
 *
 * @param file_path The path to the template file.
 * @param context The cJSON object containing the data for the template.
 * @return A new cwist_sstring containing the rendered output. The caller is
 *         responsible for destroying the returned string. Returns NULL if the
 *         file cannot be read or on rendering failure.
 */
cwist_sstring *cwist_template_render_file(const char *file_path, const cJSON *context);

/**
 * @brief Render a string with optional diagnostics.
 * @param template_str Template source, as for cwist_template_render().
 * @param context JSON rendering context; may be NULL.
 * @param error Optional output diagnostic; may be NULL.
 * @return Rendered string owned by the caller, or NULL on failure.
 */
cwist_sstring *cwist_template_render_ex(const char *template_str, const cJSON *context,
                                        cwist_template_error_t *error);

/**
 * @brief Render a file with optional diagnostics, including nested includes.
 * @param file_path Template path, as for cwist_template_render_file().
 * @param context JSON rendering context; may be NULL.
 * @param error Optional output diagnostic; may be NULL.
 * @return Rendered string owned by the caller, or NULL on failure.
 */
cwist_sstring *cwist_template_render_file_ex(const char *file_path, const cJSON *context,
                                             cwist_template_error_t *error);

/**
 * @brief Render a string, confining every {% include %} to @p include_root.
 *
 * Relative includes resolve against @p include_root (an in-memory string has
 * no directory of its own). Each included file is canonicalized (symlinks and
 * ".." resolved) and rejected with CWIST_TEMPLATE_ERROR_FORBIDDEN unless it
 * lies inside the canonical @p include_root. The root does not change which
 * file an include names; it only rejects files outside it.
 *
 * @param template_str Template source, as for cwist_template_render().
 * @param context JSON rendering context; may be NULL.
 * @param include_root Existing directory that includes must stay inside.
 *                     A missing or NULL root fails with
 *                     CWIST_TEMPLATE_ERROR_INVALID_ARGUMENT.
 * @param error Optional output diagnostic; may be NULL.
 * @return Rendered string owned by the caller, or NULL on failure.
 */
cwist_sstring *cwist_template_render_rooted(const char *template_str, const cJSON *context,
                                            const char *include_root,
                                            cwist_template_error_t *error);

/**
 * @brief Render a file, confining every nested {% include %} to @p include_root.
 *
 * @p file_path itself is opened as given; the root applies to the files it
 * includes, at any depth. Relative includes still resolve against the
 * including file's directory, exactly as in cwist_template_render_file().
 * See cwist_template_render_rooted() for how the root is enforced.
 *
 * @param file_path Template path, as for cwist_template_render_file().
 * @param context JSON rendering context; may be NULL.
 * @param include_root Existing directory that includes must stay inside.
 * @param error Optional output diagnostic; may be NULL.
 * @return Rendered string owned by the caller, or NULL on failure.
 */
cwist_sstring *cwist_template_render_file_rooted(const char *file_path, const cJSON *context,
                                                 const char *include_root,
                                                 cwist_template_error_t *error);

#endif
