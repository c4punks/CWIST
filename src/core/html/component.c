/**
 * @file component.c
 * @brief Named render functions that produce cwist_html_element_t trees.
 */

#include <cwist/core/html/component.h>
#include <cwist/core/mem/alloc.h>
#include <stdbool.h>
#include <string.h>

struct cwist_html_component {
    char *name;
    cwist_html_component_render_fn render_fn;
};

/**
 * @brief Create a named component wrapping a render function.
 * @param name Non-empty component name; copied into the new component.
 * @param render_fn Render function invoked by cwist_html_component_instantiate().
 * @return New component on success, NULL if arguments are invalid or
 *         allocation fails. Caller owns the returned component and must
 *         release it with cwist_html_component_destroy().
 */
cwist_html_component_t *cwist_html_component_create(const char *name,
                                                    cwist_html_component_render_fn render_fn) {
    if (!name || !*name || !render_fn) return NULL;

    cwist_html_component_t *comp =
        (cwist_html_component_t *)cwist_alloc(sizeof(cwist_html_component_t));
    if (!comp) return NULL;

    comp->name = cwist_strdup(name);
    if (!comp->name) {
        cwist_free(comp);
        return NULL;
    }
    comp->render_fn = render_fn;
    return comp;
}

/**
 * @brief Destroy a component and free its name.
 * @param comp Component to destroy; NULL is accepted and does nothing.
 *           Must not be used after this call.
 */
void cwist_html_component_destroy(cwist_html_component_t *comp) {
    if (!comp) return;
    cwist_free(comp->name);
    cwist_free(comp);
}

/**
 * @brief Get the component's name.
 * @param comp Component to query; may be NULL.
 * @return The component's name, or NULL if @p comp is NULL.
 */
const char *cwist_html_component_name(const cwist_html_component_t *comp) {
    return comp ? comp->name : NULL;
}

/**
 * @brief Run the component's render function to produce an element tree.
 * @param comp Component whose render function is invoked; may be NULL.
 * @param props Opaque props forwarded to the render function.
 * @param children Child elements forwarded to the render function. When
 *        @p comp is NULL they are destroyed here instead, since no render
 *        function takes ownership of them.
 * @param child_count Number of entries in @p children.
 * @return Rendered element tree, or NULL if arguments are invalid or the
 *         render function fails. Ownership follows the render function's
 *         contract.
 */
cwist_html_element_t *cwist_html_component_instantiate(cwist_html_component_t *comp,
                                                       const void *props,
                                                       cwist_html_element_t **children,
                                                       size_t child_count) {
    if (!children) child_count = 0;
    if (!comp || !comp->render_fn) {
        release_unattached(NULL, children, child_count);
        return NULL;
    }

    cwist_html_element_t *root = comp->render_fn(props, children, child_count);
    release_unattached(root, children, child_count);
    return root;
}
