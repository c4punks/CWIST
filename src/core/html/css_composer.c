/**
 * @file css_composer.c
 * @brief Dynamic CSS Synthesis and Color Mathematics Implementation.
 */

#include <cwist/core/html/css_composer.h>
#include <cwist/core/mem/alloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#define MIN3(a, b, c) ((a) < (b) ? ((a) < (c) ? (a) : (c)) : ((b) < (c) ? (b) : (c)))
#define MAX3(a, b, c) ((a) > (b) ? ((a) > (c) ? (a) : (c)) : ((b) > (c) ? (b) : (c)))

cwist_color_hsl cwist_color_rgb_to_hsl(cwist_color_rgb rgb) {
    cwist_color_hsl hsl = {0.0f, 0.0f, 0.0f};
    float r = rgb.r / 255.0f;
    float g = rgb.g / 255.0f;
    float b = rgb.b / 255.0f;

    float max = MAX3(r, g, b);
    float min = MIN3(r, g, b);
    hsl.l = (max + min) / 2.0f;

    if (max == min) {
        hsl.h = 0.0f;
        hsl.s = 0.0f;
    } else {
        float d = max - min;
        hsl.s = hsl.l > 0.5f ? d / (2.0f - max - min) : d / (max + min);

        if (max == r) {
            hsl.h = (g - b) / d + (g < b ? 6.0f : 0.0f);
        } else if (max == g) {
            hsl.h = (b - r) / d + 2.0f;
        } else {
            hsl.h = (r - g) / d + 4.0f;
        }
        hsl.h /= 6.0f;
    }
    hsl.h *= 360.0f;
    return hsl;
}

static float hue2rgb(float p, float q, float t) {
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 1.0f / 2.0f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

cwist_color_rgb cwist_color_hsl_to_rgb(cwist_color_hsl hsl) {
    cwist_color_rgb rgb = {0, 0, 0};
    float h = hsl.h / 360.0f;
    float s = hsl.s;
    float l = hsl.l;

    if (s == 0.0f) {
        rgb.r = rgb.g = rgb.b = (unsigned char)(l * 255.0f);
    } else {
        float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
        float p = 2.0f * l - q;
        rgb.r = (unsigned char)(roundf(hue2rgb(p, q, h + 1.0f / 3.0f) * 255.0f));
        rgb.g = (unsigned char)(roundf(hue2rgb(p, q, h) * 255.0f));
        rgb.b = (unsigned char)(roundf(hue2rgb(p, q, h - 1.0f / 3.0f) * 255.0f));
    }
    return rgb;
}

static inline int hex_char_to_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

cwist_color_rgb cwist_color_hex_to_rgb(const char *hex) {
    cwist_color_rgb rgb = {0, 0, 0};
    if (!hex) return rgb;
    if (hex[0] == '#') hex++;
    size_t len = strlen(hex);
    if (len == 6) {
        int r1 = hex_char_to_val(hex[0]);
        int r2 = hex_char_to_val(hex[1]);
        int g1 = hex_char_to_val(hex[2]);
        int g2 = hex_char_to_val(hex[3]);
        int b1 = hex_char_to_val(hex[4]);
        int b2 = hex_char_to_val(hex[5]);
        if (r1 >= 0 && r2 >= 0 && g1 >= 0 && g2 >= 0 && b1 >= 0 && b2 >= 0) {
            rgb.r = (unsigned char)((r1 << 4) | r2);
            rgb.g = (unsigned char)((g1 << 4) | g2);
            rgb.b = (unsigned char)((b1 << 4) | b2);
        }
    } else if (len == 3) {
        int r = hex_char_to_val(hex[0]);
        int g = hex_char_to_val(hex[1]);
        int b = hex_char_to_val(hex[2]);
        if (r >= 0 && g >= 0 && b >= 0) {
            rgb.r = (unsigned char)((r << 4) | r);
            rgb.g = (unsigned char)((g << 4) | g);
            rgb.b = (unsigned char)((b << 4) | b);
        }
    }
    return rgb;
}

void cwist_color_rgb_to_hex(cwist_color_rgb rgb, char out_hex[8]) {
    if (out_hex) {
        snprintf(out_hex, 8, "#%02x%02x%02x", rgb.r, rgb.g, rgb.b);
    }
}

void cwist_css_config_init(cwist_css_config *cfg) {
    if (!cfg) return;
    cfg->primary_color = cwist_color_hex_to_rgb("#3B82F6"); // Default Tailwind Blue
    cfg->secondary_color = cwist_color_hex_to_rgb("#10B981"); // Default Tailwind Green
    cfg->roundness_px = 8.0f;
    cfg->spacing_base_px = 4.0f;
    cfg->is_dark_mode = false;
}

/**
 * @brief Interpolates a new color by altering lightness.
 */
static void get_shifted_hex(cwist_color_rgb base, float light_shift, char out[8]) {
    cwist_color_hsl hsl = cwist_color_rgb_to_hsl(base);
    hsl.l += light_shift;
    if (hsl.l > 1.0f) hsl.l = 1.0f;
    if (hsl.l < 0.0f) hsl.l = 0.0f;
    cwist_color_rgb shifted = cwist_color_hsl_to_rgb(hsl);
    cwist_color_rgb_to_hex(shifted, out);
}

cwist_sstring *cwist_css_generate_variables(const cwist_css_config *cfg) {
    if (!cfg) return NULL;
    cwist_sstring *css = cwist_sstring_create();

    char p_hex[8], p_hover[8], p_active[8];
    char s_hex[8], s_hover[8], s_active[8];

    cwist_color_rgb_to_hex(cfg->primary_color, p_hex);
    get_shifted_hex(cfg->primary_color, cfg->is_dark_mode ? 0.1f : -0.1f, p_hover);
    get_shifted_hex(cfg->primary_color, cfg->is_dark_mode ? 0.15f : -0.15f, p_active);

    cwist_color_rgb_to_hex(cfg->secondary_color, s_hex);
    get_shifted_hex(cfg->secondary_color, cfg->is_dark_mode ? 0.1f : -0.1f, s_hover);
    get_shifted_hex(cfg->secondary_color, cfg->is_dark_mode ? 0.15f : -0.15f, s_active);

    char buf[1024];
    snprintf(buf, sizeof(buf),
             ":root {\n"
             "  --color-primary: %s;\n"
             "  --color-primary-hover: %s;\n"
             "  --color-primary-active: %s;\n"
             "  --color-secondary: %s;\n"
             "  --color-secondary-hover: %s;\n"
             "  --color-secondary-active: %s;\n"
             "  --bg-body: %s;\n"
             "  --bg-surface: %s;\n"
             "  --text-main: %s;\n"
             "  --text-muted: %s;\n"
             "  --radius-sm: %.1fpx;\n"
             "  --radius-md: %.1fpx;\n"
             "  --radius-lg: %.1fpx;\n"
             "  --space-1: %.1fpx;\n"
             "  --space-2: %.1fpx;\n"
             "  --space-4: %.1fpx;\n"
             "  --space-8: %.1fpx;\n"
             "}\n",
             p_hex, p_hover, p_active, s_hex, s_hover, s_active,
             cfg->is_dark_mode ? "#121212" : "#FFFFFF", cfg->is_dark_mode ? "#1E1E1E" : "#F3F4F6",
             cfg->is_dark_mode ? "#F9FAFB" : "#111827", cfg->is_dark_mode ? "#9CA3AF" : "#6B7280",
             cfg->roundness_px * 0.5f, cfg->roundness_px, cfg->roundness_px * 1.5f,
             cfg->spacing_base_px, cfg->spacing_base_px * 2.0f, cfg->spacing_base_px * 4.0f,
             cfg->spacing_base_px * 8.0f);

    cwist_sstring_append(css, buf);
    return css;
}

cwist_sstring *cwist_css_generate_utility_classes(const cwist_css_config *cfg) {
    (void)cfg; // Currently relying on generated variables
    cwist_sstring *css = cwist_sstring_create();

    const char *utils = "/* Typography */\n"
                        ".text-primary { color: var(--color-primary); }\n"
                        ".text-main { color: var(--text-main); }\n"
                        ".text-muted { color: var(--text-muted); }\n"
                        "\n"
                        "/* Backgrounds & Surfaces */\n"
                        ".bg-body { background-color: var(--bg-body); }\n"
                        ".bg-surface { background-color: var(--bg-surface); }\n"
                        ".bg-primary { background-color: var(--color-primary); color: #fff; }\n"
                        ".bg-primary:hover { background-color: var(--color-primary-hover); }\n"
                        ".bg-primary:active { background-color: var(--color-primary-active); }\n"
                        "\n"
                        "/* Components */\n"
                        ".btn {\n"
                        "  display: inline-flex;\n"
                        "  align-items: center;\n"
                        "  justify-content: center;\n"
                        "  padding: var(--space-2) var(--space-4);\n"
                        "  border-radius: var(--radius-md);\n"
                        "  font-weight: 500;\n"
                        "  transition: background-color 0.2s, color 0.2s;\n"
                        "  cursor: pointer;\n"
                        "  border: none;\n"
                        "}\n"
                        "\n"
                        ".card {\n"
                        "  background-color: var(--bg-surface);\n"
                        "  border-radius: var(--radius-lg);\n"
                        "  padding: var(--space-4);\n"
                        "  box-shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.1);\n"
                        "}\n";

    cwist_sstring_append(css, utils);
    return css;
}

cwist_sstring *cwist_css_generate_stylesheet(const cwist_css_config *cfg) {
    if (!cfg) return NULL;
    cwist_sstring *final_css = cwist_sstring_create();

    cwist_sstring *vars = cwist_css_generate_variables(cfg);
    cwist_sstring *utils = cwist_css_generate_utility_classes(cfg);

    if (vars) cwist_sstring_append(final_css, vars->data);
    if (utils) cwist_sstring_append(final_css, utils->data);

    if (vars) cwist_sstring_destroy(vars);
    if (utils) cwist_sstring_destroy(utils);

    return final_css;
}

struct cwist_css_scope_entry {
    char *base_class;                   ///< Lookup key ("btn")
    cwist_sstring *scoped_class;        ///< "btn-<suffix>", owned
    char *declarations;                 ///< NULL until cwist_css_scope_add_rule()
    bool used;
};

/**
 * @brief Append to an sstring, collapsing the cwist_error_t ceremony into a
 *        bool so construction sites can chain with &&.
 */
static bool append_ok(cwist_sstring *s, const char *text) {
    cwist_error_t err = cwist_sstring_append(s, text);
    bool ok = cwist_error_is_ok(&err);
    cwist_error_dispose(&err);
    return ok;
}

static bool is_css_ident(const char *s) {
    if (!s || !*s) return false;
    const char *p = s;
    if (*p == '-') p++;
    if (!(isalpha((unsigned char)*p) || *p == '_')) return false;
    for (p++; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-')) return false;
    }
    return true;
}

static struct cwist_css_scope_entry *scope_find(const cwist_css_scope *scope,
                                                const char *base_class) {
    for (size_t i = 0; i < scope->count; i++) {
        if (strcmp(scope->entries[i].base_class, base_class) == 0) return &scope->entries[i];
    }
    return NULL;
}

/**
 * @brief Find the entry for `base_class`, creating it (with its scoped name)
 *        when it does not exist yet.
 */
static struct cwist_css_scope_entry *scope_get_or_add(cwist_css_scope *scope,
                                                      const char *base_class) {
    struct cwist_css_scope_entry *entry = scope_find(scope, base_class);
    if (entry) return entry;

    if (scope->count == scope->capacity) {
        size_t new_capacity = scope->capacity ? scope->capacity * 2 : 8;
        struct cwist_css_scope_entry *grown = (struct cwist_css_scope_entry *)cwist_realloc(
            scope->entries, new_capacity * sizeof(*grown));
        if (!grown) return NULL;
        scope->entries = grown;
        scope->capacity = new_capacity;
    }

    char *base_copy = cwist_strdup(base_class);
    cwist_sstring *scoped = cwist_sstring_create();
    if (!base_copy || !scoped || !append_ok(scoped, base_class) || !append_ok(scoped, "-") ||
        !append_ok(scoped, scope->suffix)) {
        cwist_free(base_copy);
        cwist_sstring_destroy(scoped);
        return NULL;
    }

    entry = &scope->entries[scope->count++];
    entry->base_class = base_copy;
    entry->scoped_class = scoped;
    entry->declarations = NULL;
    entry->used = false;
    return entry;
}

void cwist_css_scope_init(cwist_css_scope *scope, const char *component_name) {
    if (!scope) return;
    const char *name = component_name ? component_name : "";

    /* FNV-1a, 32-bit. Deliberately unseeded: markup and stylesheet may be
     * produced by different worker processes and must agree on the suffix. */
    uint32_t hash = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        hash ^= *p;
        hash *= 16777619u;
    }

    snprintf(scope->suffix, sizeof(scope->suffix), "%08x", (unsigned int)hash);
    scope->entries = NULL;
    scope->count = 0;
    scope->capacity = 0;
}

const char *cwist_css_scope_class(cwist_css_scope *scope, const char *base_class) {
    if (!scope || !scope->suffix[0] || !is_css_ident(base_class)) return NULL;
    struct cwist_css_scope_entry *entry = scope_get_or_add(scope, base_class);
    if (!entry) return NULL;
    entry->used = true;
    return entry->scoped_class->data;
}

int cwist_css_scope_add_rule(cwist_css_scope *scope, const char *base_class,
                             const char *declarations) {
    if (!scope || !scope->suffix[0] || !is_css_ident(base_class) || !declarations) return -1;
    if (strpbrk(declarations, "{}<")) return -1;

    char *copy = cwist_strdup(declarations);
    if (!copy) return -1;
    struct cwist_css_scope_entry *entry = scope_get_or_add(scope, base_class);
    if (!entry) {
        cwist_free(copy);
        return -1;
    }
    cwist_free(entry->declarations);
    entry->declarations = copy;
    return 0;
}

cwist_sstring *cwist_css_scope_generate_stylesheet(const cwist_css_scope *scope) {
    if (!scope) return NULL;
    cwist_sstring *css = cwist_sstring_create();
    if (!css) return NULL;

    cwist_error_t err = cwist_sstring_assign(css, "");
    bool ok = cwist_error_is_ok(&err);
    cwist_error_dispose(&err);

    for (size_t i = 0; ok && i < scope->count; i++) {
        const struct cwist_css_scope_entry *entry = &scope->entries[i];
        if (!entry->used || !entry->declarations) continue;
        ok = append_ok(css, ".") && append_ok(css, entry->scoped_class->data) &&
             append_ok(css, " { ") && append_ok(css, entry->declarations) && append_ok(css, " }\n");
    }
    if (!ok) {
        cwist_sstring_destroy(css);
        return NULL;
    }
    return css;
}

void cwist_css_scope_destroy(cwist_css_scope *scope) {
    if (!scope) return;
    for (size_t i = 0; i < scope->count; i++) {
        cwist_free(scope->entries[i].base_class);
        cwist_sstring_destroy(scope->entries[i].scoped_class);
        cwist_free(scope->entries[i].declarations);
    }
    cwist_free(scope->entries);
    memset(scope, 0, sizeof(*scope));
}
