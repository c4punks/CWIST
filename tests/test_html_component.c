#include <cwist/core/html/component.h>
#include <cwist/core/html/builder.h>
#include <cwist/core/html/css_composer.h>
#include <cwist/core/sstring/sstring.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

typedef struct {
    const char *title;
    cwist_css_scope *scope;
} card_props;

static int render_calls;

static void destroy_children(cwist_html_element_t **children, size_t from, size_t count) {
    for (size_t i = from; i < count; i++) {
        cwist_html_element_destroy(children[i]);
    }
}

/* <div class="card"><h2>title</h2>children...</div> */
static cwist_html_element_t *card_render(const void *props, cwist_html_element_t **children,
                                         size_t child_count) {
    const card_props *p = (const card_props *)props;
    render_calls++;

    cwist_html_element_t *root = cwist_html_element_create("div");
    if (!root) {
        destroy_children(children, 0, child_count);
        return NULL;
    }
    const char *cls = p && p->scope ? cwist_css_scope_class(p->scope, "card") : "card";
    cwist_html_element_add_class(root, cls);

    cwist_html_element_t *h2 = cwist_html_element_create("h2");
    cwist_html_element_set_text(h2, p && p->title ? p->title : "");
    cwist_html_element_add_child(root, h2);

    for (size_t i = 0; i < child_count; i++) {
        cwist_html_element_add_child(root, children[i]);
    }
    return root;
}

/* Keeps only the first child and destroys the rest, as the contract requires. */
static cwist_html_element_t *first_only_render(const void *props, cwist_html_element_t **children,
                                               size_t child_count) {
    (void)props;
    cwist_html_element_t *root = cwist_html_element_create("section");
    if (!root) {
        destroy_children(children, 0, child_count);
        return NULL;
    }
    if (child_count > 0) cwist_html_element_add_child(root, children[0]);
    destroy_children(children, 1, child_count);
    return root;
}

/* Fails after attaching the first child: destroying the partial tree releases
 * that child, and only the unattached rest need an explicit destroy. */
static cwist_html_element_t *
partial_failure_render(const void *props, cwist_html_element_t **children, size_t child_count) {
    (void)props;
    render_calls++;
    cwist_html_element_t *root = cwist_html_element_create("div");
    if (root && child_count > 0) cwist_html_element_add_child(root, children[0]);
    cwist_html_element_destroy(root);
    destroy_children(children, root ? 1 : 0, child_count);
    return NULL;
}

/* Returns its only child as the root. */
static cwist_html_element_t *passthrough_render(const void *props, cwist_html_element_t **children,
                                                size_t child_count) {
    (void)props;
    if (child_count == 1) return children[0];
    destroy_children(children, 0, child_count);
    return NULL;
}

static cwist_html_element_t *text_el(const char *tag, const char *text) {
    cwist_html_element_t *el = cwist_html_element_create(tag);
    assert(el != NULL);
    cwist_html_element_set_text(el, text);
    return el;
}

static void test_create_validation(void) {
    assert(cwist_html_component_create(NULL, card_render) == NULL);
    assert(cwist_html_component_create("", card_render) == NULL);
    assert(cwist_html_component_create("card", NULL) == NULL);

    cwist_html_component_t *comp = cwist_html_component_create("card", card_render);
    assert(comp != NULL);
    assert(strcmp(cwist_html_component_name(comp), "card") == 0);
    assert(cwist_html_component_name(NULL) == NULL);
    cwist_html_component_destroy(comp);
    cwist_html_component_destroy(NULL);
    printf("Passed test_create_validation\n");
}

static void test_instantiate_with_children(void) {
    cwist_html_component_t *card = cwist_html_component_create("card", card_render);
    assert(card != NULL);

    card_props props = {"Hello <world>", NULL};
    cwist_html_element_t *kids[2] = {text_el("p", "one"), text_el("p", "two")};
    cwist_html_element_t *root = cwist_html_component_instantiate(card, &props, kids, 2);
    assert(root != NULL);

    cwist_sstring *html = cwist_html_render(root);
    assert(html != NULL && html->data != NULL);
    assert(strcmp(html->data, "<div class=\"card\"><h2>Hello &lt;world&gt;</h2>"
                              "<p>one</p><p>two</p></div>") == 0);
    cwist_sstring_destroy(html);

    /* No props and no children is a valid instance too. */
    cwist_html_element_t *bare = cwist_html_component_instantiate(card, NULL, NULL, 0);
    assert(bare != NULL);
    html = cwist_html_render(bare);
    assert(strcmp(html->data, "<div class=\"card\"><h2></h2></div>") == 0);
    cwist_sstring_destroy(html);

    cwist_html_element_destroy(bare);
    cwist_html_element_destroy(root);
    cwist_html_component_destroy(card);
    printf("Passed test_instantiate_with_children\n");
}

static void test_nested_instances(void) {
    cwist_html_component_t *card = cwist_html_component_create("card", card_render);
    card_props inner_props = {"inner", NULL};
    card_props outer_props = {"outer", NULL};

    cwist_html_element_t *inner_kids[1] = {text_el("span", "x")};
    cwist_html_element_t *inner =
        cwist_html_component_instantiate(card, &inner_props, inner_kids, 1);
    assert(inner != NULL);
    cwist_html_element_t *outer_kids[1] = {inner};
    cwist_html_element_t *outer =
        cwist_html_component_instantiate(card, &outer_props, outer_kids, 1);
    assert(outer != NULL);

    cwist_sstring *html = cwist_html_render(outer);
    assert(strcmp(html->data, "<div class=\"card\"><h2>outer</h2>"
                              "<div class=\"card\"><h2>inner</h2><span>x</span></div></div>") == 0);
    cwist_sstring_destroy(html);
    cwist_html_element_destroy(outer);
    cwist_html_component_destroy(card);
    printf("Passed test_nested_instances\n");
}

/* The ownership cases below rely on the sanitizer build (LeakSanitizer and
 * double-free detection) to prove every child is released exactly once. */
static void test_render_drops_children(void) {
    cwist_html_component_t *comp = cwist_html_component_create("first", first_only_render);
    cwist_html_element_t *kids[3] = {text_el("p", "kept"), text_el("p", "dropped"), NULL};
    cwist_html_element_t *root = cwist_html_component_instantiate(comp, NULL, kids, 3);
    assert(root != NULL);

    cwist_sstring *html = cwist_html_render(root);
    assert(strcmp(html->data, "<section><p>kept</p></section>") == 0);
    cwist_sstring_destroy(html);
    cwist_html_element_destroy(root);
    cwist_html_component_destroy(comp);
    printf("Passed test_render_drops_children\n");
}

static void test_failure_paths(void) {
    /* A render function that cleans up a partial tree must not be second-guessed. */
    render_calls = 0;
    cwist_html_component_t *comp = cwist_html_component_create("fail", partial_failure_render);
    cwist_html_element_t *kids[3] = {text_el("p", "a"), text_el("p", "b"), text_el("p", "c")};
    assert(cwist_html_component_instantiate(comp, NULL, kids, 3) == NULL);
    assert(render_calls == 1);

    /* A NULL component still consumes the children. */
    cwist_html_element_t *more[2] = {text_el("p", "d"), NULL};
    assert(cwist_html_component_instantiate(NULL, NULL, more, 2) == NULL);

    /* A non-zero count without an array is rejected before rendering. */
    render_calls = 0;
    assert(cwist_html_component_instantiate(comp, NULL, NULL, 5) == NULL);
    assert(cwist_html_component_instantiate(NULL, NULL, NULL, 5) == NULL);
    assert(render_calls == 0);

    cwist_html_component_destroy(comp);
    printf("Passed test_failure_paths\n");
}

static void test_child_returned_as_root(void) {
    cwist_html_component_t *comp = cwist_html_component_create("pass", passthrough_render);
    cwist_html_element_t *kids[1] = {text_el("em", "only")};
    cwist_html_element_t *root = cwist_html_component_instantiate(comp, NULL, kids, 1);
    assert(root == kids[0]);

    cwist_sstring *html = cwist_html_render(root);
    assert(strcmp(html->data, "<em>only</em>") == 0);
    cwist_sstring_destroy(html);
    cwist_html_element_destroy(root);
    cwist_html_component_destroy(comp);
    printf("Passed test_child_returned_as_root\n");
}

static void test_component_with_scoped_css(void) {
    cwist_html_component_t *card = cwist_html_component_create("card", card_render);
    cwist_css_scope scope;
    cwist_css_scope_init(&scope, cwist_html_component_name(card));
    assert(cwist_css_scope_add_rule(&scope, "card", "padding: 8px;") == 0);

    card_props props = {"t", &scope};
    cwist_html_element_t *root = cwist_html_component_instantiate(card, &props, NULL, 0);
    assert(root != NULL);

    /* FNV-1a("card") == 0x8827595f */
    cwist_sstring *html = cwist_html_render(root);
    assert(strcmp(html->data, "<div class=\"card-8827595f\"><h2>t</h2></div>") == 0);
    cwist_sstring *css = cwist_css_scope_generate_stylesheet(&scope);
    assert(strcmp(css->data, ".card-8827595f { padding: 8px; }\n") == 0);

    cwist_sstring_destroy(css);
    cwist_sstring_destroy(html);
    cwist_html_element_destroy(root);
    cwist_css_scope_destroy(&scope);
    cwist_html_component_destroy(card);
    printf("Passed test_component_with_scoped_css\n");
}

int main(void) {
    test_create_validation();
    test_instantiate_with_children();
    test_nested_instances();
    test_render_drops_children();
    test_failure_paths();
    test_child_returned_as_root();
    test_component_with_scoped_css();
    printf("All HTML component tests passed!\n");
    return 0;
}
