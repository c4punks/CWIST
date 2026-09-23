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

/**
 * @brief Report whether `target` is `root` or one of its descendants.
 */
static bool tree_contains(const cwist_html_element_t *root, const cwist_html_element_t *target) {
    if (!root) return false;
    if (root == target) return true;
    for (int i = 0; i < root->child_count; i++) {
        if (tree_contains(root->children[i], target)) return true;
    }
    return false;
}

/**
 * @brief Destroy every child not reachable from `root` (all of them when
 *        `root` is NULL), releasing a pointer listed twice only once.
 */
static void release_unattached(const cwist_html_element_t *root, cwist_html_element_t **children,
                               size_t child_count) {
    if (!children) return;
    for (size_t i = 0; i < child_count; i++) {
        cwist_html_element_t *child = children[i];
        if (!child) continue;

        bool seen = false;
        for (size_t j = 0; j < i; j++) {
            if (children[j] == child) {
                seen = true;
                break;
            }
        }
        if (seen || tree_contains(root, child)) continue;
        cwist_html_element_destroy(child);
    }
}

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
