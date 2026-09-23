# HTML Components, Scoped CSS, Fragments and Assets

*Headers:* `<cwist/core/html/component.h>`, `<cwist/core/html/css_composer.h>`

Components are named render functions over the element builder in
`<cwist/core/html/builder.h>`. A CSS scope hands out class names specific to
one component and emits the rules for the classes that were actually used.
Both are plain C with no socket or thread dependencies and are part of the WASM
build.

## Components

### `cwist_html_component_create` / `cwist_html_component_destroy`
```c
typedef cwist_html_element_t *(*cwist_html_component_render_fn)(const void *props,
                                                                cwist_html_element_t **children,
                                                                size_t child_count);

cwist_html_component_t *cwist_html_component_create(const char *name,
                                                    cwist_html_component_render_fn render_fn);
void cwist_html_component_destroy(cwist_html_component_t *comp);
const char *cwist_html_component_name(const cwist_html_component_t *comp);
```
`name` must be non-empty and is copied. Destroying a component does not affect
element trees it already produced.

### `cwist_html_component_instantiate`
```c
cwist_html_element_t *cwist_html_component_instantiate(cwist_html_component_t *comp,
                                                       const void *props,
                                                       cwist_html_element_t **children,
                                                       size_t child_count);
```
Runs the render function and returns its root element. The result is owned by
the caller exactly like the result of `cwist_html_element_create()`: destroy it
with `cwist_html_element_destroy()` or attach it to a parent.

The caller gives up every element in `children` when it calls this function
and must not use them afterwards. They are handed to the render function, or
destroyed here if `comp` is NULL. Each element must be listed once and must not
already belong to another tree, including another entry's. `children` may be
NULL only when `child_count` is 0; a NULL array with a non-zero count returns
NULL without rendering or releasing anything. Individual entries may be NULL.

The render function owns the children it receives. Each one must end up in the
tree it returns (attached with `cwist_html_element_add_child()`, or returned as
the root) or be destroyed by the render function, whether it succeeds or not.
On failure, destroying a partially built tree also releases the children
already attached to it, so only the unattached ones still need
`cwist_html_element_destroy()`.

`props` is passed through untouched, so a component defines its own props
struct.

## Scoped CSS

### `cwist_css_scope_init` / `cwist_css_scope_destroy`
```c
void cwist_css_scope_init(cwist_css_scope *scope, const char *component_name);
void cwist_css_scope_destroy(cwist_css_scope *scope);
```
The scope is a value struct in caller storage. Every class it hands out gets the
suffix `-XXXXXXXX`, the 32-bit FNV-1a hash of `component_name` in lowercase hex.
The hash is unseeded on purpose: the same component name gives the same class
names in every process and every run, so markup and stylesheet can come from
different prefork workers. It is not a security boundary, and two component
names can in principle share a suffix.

`cwist_css_scope_destroy()` frees everything, invalidates all returned class
names, and leaves the scope empty. Calling it twice is harmless. A destroyed or
zero-initialised scope rejects new classes and rules until
`cwist_css_scope_init()` is called again.

### `cwist_css_scope_class`
```c
const char *cwist_css_scope_class(cwist_css_scope *scope, const char *base_class);
```
Returns the scoped name (`"btn"` -> `"btn-a1b2c3d4"`) and marks the class as
used. The same base class always returns the same pointer, owned by the scope.
`base_class` must be a plain ASCII identifier: a letter or `_` (optionally after
one leading `-`), then letters, digits, `_` or `-`. This is a deliberate subset
of the CSS identifier grammar: escapes, non-ASCII characters and `--` names are
rejected. Anything else returns NULL.

### `cwist_css_scope_add_rule`
```c
int cwist_css_scope_add_rule(cwist_css_scope *scope, const char *base_class,
                             const char *declarations);
```
Attaches a declaration block body to a class, replacing any earlier one.
Returns 0 on success, -1 on invalid input or allocation failure.

Declarations are trusted, application-authored CSS and are emitted verbatim.
Two characters are checked. `<` is rejected, so the generated stylesheet can
never end an enclosing `<style>` element. `{` and `}` are rejected to catch
nested blocks. Nothing else is validated: an unterminated comment or string
can still affect the rules after it. Do not build declarations from untrusted
input.

### `cwist_css_scope_generate_stylesheet`
```c
cwist_sstring *cwist_css_scope_generate_stylesheet(const cwist_css_scope *scope);
```
Emits `.<scoped> { <declarations> }` for each class that was both requested with
`cwist_css_scope_class()` and given a rule, in the order the scope first saw
each class. Rules for classes that were never requested are left out. The
caller destroys the returned string. NULL is returned when `scope` is NULL or
an allocation fails, never a partial stylesheet.

Only plain class selectors are generated; pseudo-classes, descendant selectors
and media queries are not covered by the scope API.

## HTML over the wire

*Header:* `<cwist/net/http/html_response.h>`

These helpers let one handler serve both a normal navigation and a fragment
request for the same URL. Without client-side script every link and form works
as a full page load; with a fragment-swapping client the same handler returns
only the part that changes.

A request is a fragment request when it carries `HX-Request: true` (htmx), except
for htmx history restores (`HX-History-Restore-Request: true`), or a non-empty
`Turbo-Frame` header (Hotwire Turbo frames). Neither library is required or
bundled.

### `cwist_http_request_wants_fragment` / `cwist_http_request_fragment_target`
```c
bool cwist_http_request_wants_fragment(const cwist_http_request *req);
const char *cwist_http_request_fragment_target(const cwist_http_request *req);
```
The target is the `Turbo-Frame` value, else `HX-Target`, and NULL for requests
that are not fragment requests.

### `cwist_http_response_set_view`
```c
int cwist_http_response_set_view(const cwist_http_request *req, cwist_http_response *res,
                                 cwist_html_element_t *content, cwist_html_component_t *layout,
                                 const void *layout_props);
```
Fragment requests get `content` alone. Other requests get a full document,
starting with `<!DOCTYPE html>`: `content` passed as the single child of the
`layout` component, or `content` itself when `layout` is NULL. Both answers get
`Content-Type: text/html; charset=utf-8` and `Vary: HX-Request,
HX-History-Restore-Request, HX-Target, Turbo-Frame`, so HTTP caches keep the two
representations apart. The in-process reply cache (Big Dumb Reply) does not
learn responses that carry `Vary`, for the same reason. The status code is left
as the handler set it.

`content` is always consumed. On a full-page answer it goes to the layout's
render function, which owns it under the usual component rules.

### `cwist_http_response_set_html`
```c
int cwist_http_response_set_html(cwist_http_response *res, cwist_html_element_t *root,
                                 bool document);
```
The lower-level step: renders `root` (always consumed) into the body, with a
doctype when `document` is true, and sets the HTML content type. It refuses
responses that already use a pointer, file or streamed body.

### `cwist_http_response_add_oob`
```c
int cwist_http_response_add_oob(const cwist_http_request *req, cwist_http_response *res,
                                cwist_html_element_t *el);
```
Adds a second region to a fragment answer, for example a counter or a
notification elsewhere on the page. On a fragment request the element (which
must have an `id`) is appended to the body with `hx-swap-oob="true"`, unless it
already has an `hx-swap-oob` value. On a full-page request it is dropped,
because the page already contains that region. htmx applies out-of-band
elements; Turbo frames ignore them. Call it after setting the main content.

### `cwist_http_response_html_redirect`
```c
int cwist_http_response_html_redirect(const cwist_http_request *req, cwist_http_response *res,
                                      const char *location);
```
An htmx request gets `200` with `HX-Redirect: <location>`. A script-driven
request follows a 3xx by itself, so a plain redirect would put the next page
inside the fragment slot. Every other request, Turbo frames included, gets
`303 See Other` with `Location`, which also turns a form POST into a GET. The
body is emptied and `Vary: HX-Request` is added. A location containing control
characters (CR and LF in particular) is refused with -1 and nothing is changed.

### Handler example

```c
static cwist_html_component_t *layout; /* created once at startup */

static void items(cwist_http_request *req, cwist_http_response *res) {
    cwist_html_element_t *list = cwist_html_element_create("ul");
    cwist_html_element_set_id(list, "items");
    /* ... add <li> children ... */
    if (cwist_http_response_set_view(req, res, list, layout, "Items") != 0) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
    }
}

static void add_item(cwist_http_request *req, cwist_http_response *res) {
    /* ... store the item ... */
    cwist_http_response_html_redirect(req, res, "/items");
}
```

With `<a href="/items" hx-get="/items" hx-target="#items">`, a browser without
htmx follows the link and receives the whole page, and htmx receives only the
`<ul id="items">` fragment from the same handler.

## Asset pipeline

*Headers:* `<cwist/core/html/css_composer.h>`, `<cwist/sys/app/assets.h>`

Stylesheets built at startup (from `cwist_css_config`, component scopes, or
files) can be bundled, minified and served under a content-hashed URL, with no
separate front-end build step.

### `cwist_css_minify` / `cwist_css_bundle`
```c
cwist_sstring *cwist_css_minify(const char *css);
cwist_sstring *cwist_css_bundle(const char *const *parts, size_t count, bool minify);
```
The minifier is deliberately conservative. It removes comments, except those
starting with `!` (license notices), and drops whitespace only where CSS
cannot need it: after `{ } ; , : > (` and before `{ } ; , > )`. It also drops
the `;` before a `}`. Every other run of whitespace becomes one space, so
`div :hover`, `and (min-width: ...)` and the spaces calc() needs around `+`
and `-` survive. Strings, escapes such as `.md\:flex`, and unquoted `url(...)`
arguments are copied verbatim. Removing a comment never joins two tokens.
Running the minifier on its own output changes nothing.

`cwist_css_bundle()` joins parts in order with a newline between them and can
minify the result. It does not resolve `@import`.

### `cwist_app_asset_add` / `cwist_app_asset_add_file` / `cwist_app_asset_url`
```c
cwist_error_t cwist_app_asset_prefix(cwist_app *app, const char *url_prefix);
cwist_error_t cwist_app_asset_add(cwist_app *app, const char *name, const void *data,
                                  size_t len, const char *content_type);
cwist_error_t cwist_app_asset_add_file(cwist_app *app, const char *name, const char *path,
                                       const char *content_type);
const char *cwist_app_asset_url(cwist_app *app, const char *name);
```
An asset registered as `css/app.css` is served at
`/assets/css/app.<16 hex digits>.css`, where the digits are the start of the
SHA-256 of its bytes, with `ETag`, `Cache-Control: public, max-age=31536000,
immutable`, and `304 Not Modified` for a matching `If-None-Match`. The same
bytes always give the same URL, in every process and across restarts. New
bytes give a new URL, and the previous URL keeps serving the previous bytes
until the app is destroyed, so pages that are still cached somewhere keep
loading their styles. The logical name (`/assets/css/app.css`) is served too,
with `Cache-Control: no-cache`, for tools that cannot know the hash.

Assets are answered on a route miss, before static directories, through the
app's middleware chain. They need no filesystem, so they work in the WASM
builds as well. The content type comes from the extension unless one is
given. Names are restricted to letters, digits, `.`, `-`, `_`, `~` and `/`
separated segments; `.` and `..` segments are rejected. Register assets
before `cwist_app_listen()`: prefork workers see only what existed when they
were forked. A port detached with `cwist_multiport_get_app()` gets a copy of
the assets registered at that point. All functions return 0 or -1 on the
`err_i16` channel.

### Putting it together

```c
/* At startup, before cwist_app_listen(). */
cwist_sstring *scoped = cwist_css_scope_generate_stylesheet(&card_css);
const char *parts[] = {base_css, scoped->data};
cwist_sstring *bundle = cwist_css_bundle(parts, 2, true);
cwist_app_asset_add(app, "app.css", bundle->data, bundle->size, NULL);
cwist_sstring_destroy(bundle);
cwist_sstring_destroy(scoped);

/* In the layout component's render function. */
cwist_html_element_t *link = cwist_html_element_create("link");
cwist_html_element_add_attr(link, "rel", "stylesheet");
cwist_html_element_add_attr(link, "href", cwist_app_asset_url(app, "app.css"));
```

## Rendering the same views in WASM

The component, scoped CSS, page/fragment and asset code above is part of the
WASM build (`libcwist_wasm.a`, and the WASI 0.2 archive built from the same
source list). An app compiled to WASM renders through the same functions as
the server, so there is one renderer, not two.

`cwist_app_dispatch_memory()` takes raw request bytes and returns raw response
bytes, and it is what `CWIST_WASM_DEFINE_ENTRY` exposes to JavaScript. That
makes one set of view code usable in three places:

- on the server, for the first page load;
- in a Service Worker, which can answer the fragment requests a page makes
  after that, with no round trip (see `example/wasm-service-worker`, route
  `GET /items/list`);
- in any other WASM host that forwards HTTP-shaped requests.

Assets registered with `cwist_app_asset_add()` are served by the WASM app too,
since they never touch the filesystem. `cwist_app_asset_add_file()` needs
whatever filesystem the host provides.

The parity is checked, not assumed. `tests/html_views_shared.h` defines a
layout and a card component, a scoped stylesheet bundled into a hashed asset,
and page, fragment, out-of-band and redirect routes, plus the exact response
bytes each request must produce. `test_html_parity` runs those checks in the
native build, and `make wasm-smoke` runs the same checks under Emscripten and
node, including the SHA-256-derived asset URL and the FNV-1a scope suffix.

## Example

```c
#include <cwist/core/html/component.h>
#include <cwist/core/html/css_composer.h>

typedef struct {
    const char *title;
    cwist_css_scope *css;
} card_props;

static cwist_html_element_t *card_render(const void *props, cwist_html_element_t **children,
                                         size_t child_count) {
    const card_props *p = props;
    cwist_html_element_t *root = cwist_html_element_create("div");
    if (!root) {
        /* The render function owns the children, including on failure. */
        for (size_t i = 0; i < child_count; i++) cwist_html_element_destroy(children[i]);
        return NULL;
    }
    cwist_html_element_add_class(root, cwist_css_scope_class(p->css, "card"));

    cwist_html_element_t *h2 = cwist_html_element_create("h2");
    cwist_html_element_set_text(h2, p->title);
    cwist_html_element_add_child(root, h2);
    for (size_t i = 0; i < child_count; i++) cwist_html_element_add_child(root, children[i]);
    return root;
}

/* ... */
cwist_html_component_t *card = cwist_html_component_create("card", card_render);
cwist_css_scope css;
cwist_css_scope_init(&css, cwist_html_component_name(card));
cwist_css_scope_add_rule(&css, "card", "padding: 1rem; border-radius: 8px;");

cwist_html_element_t *body = cwist_html_element_create("p");
cwist_html_element_set_text(body, "Hello");
card_props props = {"Welcome", &css};
cwist_html_element_t *el = cwist_html_component_instantiate(card, &props, &body, 1);

cwist_sstring *html = cwist_html_render(el);             /* <div class="card-8827595f">... */
cwist_sstring *style = cwist_css_scope_generate_stylesheet(&css);
/* .card-8827595f { padding: 1rem; border-radius: 8px; } */

cwist_sstring_destroy(style);
cwist_sstring_destroy(html);
cwist_html_element_destroy(el);
cwist_css_scope_destroy(&css);
cwist_html_component_destroy(card);
```
