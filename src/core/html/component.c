/**
 * @file component.c
 * @brief Named render functions that produce cwist_html_element_t trees.
 */

#include <cwist/core/html/component.h>
#include <cwist/core/mem/alloc.h>

struct cwist_html_component {
    char *name;
    cwist_html_component_render_fn render_fn;
};

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

void cwist_html_component_destroy(cwist_html_component_t *comp) {
    if (!comp) return;
    cwist_free(comp->name);
    cwist_free(comp);
}

const char *cwist_html_component_name(const cwist_html_component_t *comp) {
    return comp ? comp->name : NULL;
}

cwist_html_element_t *cwist_html_component_instantiate(cwist_html_component_t *comp,
                                                       const void *props,
                                                       cwist_html_element_t **children,
                                                       size_t child_count) {
    if (!children && child_count > 0) return NULL;
    if (!comp) {
        /* No render function will take the children, so release them here. */
        for (size_t i = 0; i < child_count; i++) {
            cwist_html_element_destroy(children[i]);
        }
        return NULL;
    }
    return comp->render_fn(props, children, child_count);
}
