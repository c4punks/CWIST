/**
 * @file component.h
 * @brief Reusable render units composed on top of the HTML element builder.
 *
 * A component pairs a name with a render function. Instantiating it runs the
 * render function over caller-supplied props and child elements and yields a
 * single root cwist_html_element_t, which is owned and destroyed exactly like
 * any element returned by cwist_html_element_create().
 */

#ifndef CWIST_HTML_COMPONENT_H
#define CWIST_HTML_COMPONENT_H

#include <cwist/core/html/builder.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque component handle. */
typedef struct cwist_html_component cwist_html_component_t;

/**
 * @brief Render callback for a component.
 *
 * Builds and returns one root element, or NULL on failure.
 *
 * The callback owns every element in `children`. Each one must end up either
 * in the returned tree (attached with cwist_html_element_add_child(), or
 * returned as the root itself) or destroyed by the callback, on success and on
 * failure alike. On failure, destroying a partially built tree also releases
 * the children already attached to it, so only the unattached ones still need
 * an explicit cwist_html_element_destroy().
 *
 * @param props Caller-defined properties, passed through unchanged (may be NULL).
 * @param children Child elements handed to this instance (NULL when
 *                 child_count is 0). Entries may be NULL.
 * @param child_count Number of entries in `children`.
 */
typedef cwist_html_element_t *(*cwist_html_component_render_fn)(const void *props,
                                                                cwist_html_element_t **children,
                                                                size_t child_count);

/**
 * @brief Create a component.
 * @param name Non-empty component name; copied. Also a natural seed for
 *             cwist_css_scope_init().
 * @param render_fn Render callback; required.
 * @return New component, or NULL on invalid arguments or allocation failure.
 */
cwist_html_component_t *cwist_html_component_create(const char *name,
                                                    cwist_html_component_render_fn render_fn);

/**
 * @brief Destroy a component. Elements it already produced are unaffected.
 * @param comp Component to release. NULL is ignored.
 */
void cwist_html_component_destroy(cwist_html_component_t *comp);

/**
 * @brief Return the name the component was created with.
 * @return Component-owned string, or NULL when `comp` is NULL.
 */
const char *cwist_html_component_name(const cwist_html_component_t *comp);

/**
 * @brief Render one instance of a component.
 *
 * The caller gives up ownership of every element in `children`: they are
 * handed to the render function (see cwist_html_component_render_fn), or
 * destroyed here when `comp` is NULL. The caller must not touch them after this
 * call. Each element must be listed once and must not be part of another
 * element's tree (including another entry's).
 *
 * @param comp Component to render.
 * @param props Properties forwarded to the render function (may be NULL).
 * @param children Child elements for this instance; may be NULL only when
 *                 child_count is 0. Entries may be NULL.
 * @param child_count Number of entries in `children`.
 * @return New element tree owned by the caller (destroy it with
 *         cwist_html_element_destroy() or hand it to a parent with
 *         cwist_html_element_add_child()), or NULL on failure. NULL is also
 *         returned, without calling the render function, when `children` is
 *         NULL but child_count is not 0; nothing is released in that case.
 */
cwist_html_element_t *cwist_html_component_instantiate(cwist_html_component_t *comp,
                                                       const void *props,
                                                       cwist_html_element_t **children,
                                                       size_t child_count);

#ifdef __cplusplus
}
#endif

#endif
