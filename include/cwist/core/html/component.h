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
 * Builds and returns one root element. The callback may attach any of
 * `children` to the tree it returns with cwist_html_element_add_child(), or
 * return one of them as the root; it must not destroy them itself. Returning
 * NULL signals failure.
 *
 * @param props Caller-defined properties, passed through unchanged (may be NULL).
 * @param children Child elements handed to this instance (may be NULL when
 *                 child_count is 0).
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
 * Ownership of every non-NULL element in `children` passes to this call,
 * whether it succeeds or not. Children the render function attached to the
 * returned tree are released with that tree; any child it left unattached is
 * destroyed before this function returns, and on failure all of them are.
 *
 * @param comp Component to render.
 * @param props Properties forwarded to the render function (may be NULL).
 * @param children Child elements for this instance (may be NULL when
 *                 child_count is 0).
 * @param child_count Number of entries in `children`.
 * @return New element tree owned by the caller (destroy it with
 *         cwist_html_element_destroy() or hand it to a parent with
 *         cwist_html_element_add_child()), or NULL on failure.
 */
cwist_html_element_t *cwist_html_component_instantiate(cwist_html_component_t *comp,
                                                       const void *props,
                                                       cwist_html_element_t **children,
                                                       size_t child_count);

#ifdef __cplusplus
}
#endif

#endif
