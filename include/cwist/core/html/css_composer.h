/**
 * @file css_composer.h
 * @brief Dynamic CSS Synthesis and Color Mathematics.
 */

#ifndef __CWIST_CSS_COMPOSER_H__
#define __CWIST_CSS_COMPOSER_H__

#include <cwist/core/sstring/sstring.h>
#include <stdbool.h>

/**
 * @brief RGB Color representation.
 */
typedef struct cwist_color_rgb {
    unsigned char r; ///< Red channel (0-255)
    unsigned char g; ///< Green channel (0-255)
    unsigned char b; ///< Blue channel (0-255)
} cwist_color_rgb;

/**
 * @brief HSL Color representation for dynamic manipulation.
 */
typedef struct cwist_color_hsl {
    float h; ///< Hue angle in degrees (0.0 to 360.0)
    float s; ///< Saturation percentage (0.0 to 1.0)
    float l; ///< Lightness percentage (0.0 to 1.0)
} cwist_color_hsl;

/**
 * @brief Dynamic CSS configuration parameters.
 * Contains base metrics used to extrapolate an entire design system.
 */
typedef struct cwist_css_config {
    cwist_color_rgb primary_color;   ///< Base primary brand color
    cwist_color_rgb secondary_color; ///< Secondary/Accent color
    float roundness_px;              ///< Base border radius in pixels (e.g. 8.0)
    float spacing_base_px;           ///< Base spacing unit in pixels (e.g. 4.0)
    bool is_dark_mode;               ///< Flag to trigger dark mode palette generation
} cwist_css_config;

struct cwist_css_scope_entry;

/**
 * @brief Component-scoped class names and the rules attached to them.
 *
 * Every class handed out by a scope carries the same suffix, derived from the
 * component name with a fixed (unseeded) 32-bit FNV-1a hash, so "btn" in scope
 * "card" is always "btn-<8 hex digits>" in every process and every run. Only
 * the fields below are public; treat them as read-only.
 */
typedef struct cwist_css_scope {
    char suffix[9];                        ///< 8 lowercase hex digits + NUL
    struct cwist_css_scope_entry *entries; ///< Per-class state, private
    size_t count;                          ///< Number of known base classes
    size_t capacity;                       ///< Allocated entries
} cwist_css_scope;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Converts an RGB color to HSL color space.
 * @param rgb The RGB color to convert.
 * @return The corresponding HSL representation.
 */
cwist_color_hsl cwist_color_rgb_to_hsl(cwist_color_rgb rgb);

/**
 * @brief Converts an HSL color back to RGB color space.
 * @param hsl The HSL color to convert.
 * @return The corresponding RGB representation.
 */
cwist_color_rgb cwist_color_hsl_to_rgb(cwist_color_hsl hsl);

/**
 * @brief Parses a hex string (e.g., "#FF0000", "FF0000", "#F00", or "F00") into an RGB struct.
 * @param hex The null-terminated hex string.
 * @return The parsed RGB color. Defaults to black on parse error.
 */
cwist_color_rgb cwist_color_hex_to_rgb(const char *hex);

/**
 * @brief Formats an RGB struct into a 7-character hex string (e.g., "#FFFFFF").
 * @param rgb The RGB color.
 * @param out_hex A character array of at least 8 bytes to hold the result.
 */
void cwist_color_rgb_to_hex(cwist_color_rgb rgb, char out_hex[8]);

/**
 * @brief Initializes a CSS configuration with sensible defaults.
 * @param cfg Pointer to the configuration struct to initialize.
 */
void cwist_css_config_init(cwist_css_config *cfg);

/**
 * @brief Generates a CSS string containing CSS Custom Properties (:root variables).
 * Extrapolates hover states, active states, and surface colors mathematically.
 * @param cfg The configuration parameters.
 * @return A dynamically allocated string containing the CSS rules. Must be destroyed.
 */
cwist_sstring *cwist_css_generate_variables(const cwist_css_config *cfg);

/**
 * @brief Generates a set of utility classes based on the design system configuration.
 * @param cfg The configuration parameters.
 * @return A dynamically allocated string containing the CSS classes. Must be destroyed.
 */
cwist_sstring *cwist_css_generate_utility_classes(const cwist_css_config *cfg);

/**
 * @brief High-level helper to generate a complete stylesheet (Variables + Utilities).
 * @param cfg The configuration parameters.
 * @return A dynamically allocated string containing the full stylesheet.
 */
cwist_sstring *cwist_css_generate_stylesheet(const cwist_css_config *cfg);

/**
 * @brief Initialise a scope for one component.
 * @param scope Caller-owned storage to initialise. Release it with
 *              cwist_css_scope_destroy().
 * @param component_name Seed for the class suffix; NULL is treated as "".
 */
void cwist_css_scope_init(cwist_css_scope *scope, const char *component_name);

/**
 * @brief Map a base class to its scoped name ("btn" -> "btn-a1b2c3d4").
 *
 * Marks the class as used, so cwist_css_scope_generate_stylesheet() emits its
 * rule. Repeated calls with the same base class return the same pointer.
 *
 * @param scope Initialised scope.
 * @param base_class CSS identifier: starts with a letter, '_' or '-' followed
 *                   by a letter or '_', then letters, digits, '_' or '-'.
 * @return Scope-owned string valid until cwist_css_scope_destroy(), or NULL
 *         for an invalid identifier or on allocation failure.
 */
const char *cwist_css_scope_class(cwist_css_scope *scope, const char *base_class);

/**
 * @brief Attach declarations to a base class, replacing any earlier ones.
 *
 * Registering a rule does not mark the class as used; a rule whose class is
 * never requested through cwist_css_scope_class() is not emitted.
 *
 * @param scope Initialised scope.
 * @param base_class CSS identifier, same rules as cwist_css_scope_class().
 * @param declarations Declaration block body, e.g. "padding: 4px; color: red;".
 *                     Must not contain '{', '}' or '<'.
 * @return 0 on success, -1 on invalid arguments or allocation failure.
 */
int cwist_css_scope_add_rule(cwist_css_scope *scope, const char *base_class,
                             const char *declarations);

/**
 * @brief Emit one rule per class that is both used and has declarations.
 *
 * Rules appear in the order their base classes were first seen by the scope.
 *
 * @param scope Initialised scope.
 * @return A dynamically allocated string (empty when nothing qualifies), or
 *         NULL when `scope` is NULL or allocation fails. Must be destroyed.
 */
cwist_sstring *cwist_css_scope_generate_stylesheet(const cwist_css_scope *scope);

/**
 * @brief Release everything the scope owns and reset it to an empty state.
 *
 * Pointers returned by cwist_css_scope_class() become invalid. Calling it
 * again on the same scope is harmless.
 *
 * @param scope Scope to release. NULL is ignored.
 */
void cwist_css_scope_destroy(cwist_css_scope *scope);

#ifdef __cplusplus
}
#endif

#endif
