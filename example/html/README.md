# HTML builder example

A small program that builds HTML with the element builder declared in
`include/cwist/core/html/builder.h`. The reusable-component layer that sits on
top of the builder is described in the [HTML API page](../../docs/api/html.md).

| Step | Shows |
|------|-------|
| [`step-1-builder`](step-1-builder/main.c) | a single element with an id, a class and text; nested elements; and an element with custom attributes |

## Build

Build the library first, from the repository root:

```sh
make
```

Then build the step:

```sh
make -C example/html/step-1-builder
```

`make examples-check` also builds it.

## Run

```sh
./example/html/step-1-builder/step-1-builder
```

Output:

```
=== HTML Builder Tutorial ===

[Simple paragraph]
<p id="intro" class="text-lg">Hello from CWIST HTML Builder!</p>

[Nested: div > ul > li*3]
<div class="container"><ul><li>Alpha</li><li>Beta</li><li>Gamma</li></ul></div>

[Anchor with href]
<a href="https://example.com" target="_blank">Visit Example.com</a>

=== Done ===
```

## What each part does

**Simple paragraph.** `cwist_html_element_create("p")` makes an element. The
program then calls `cwist_html_element_set_id` to set `id="intro"`,
`cwist_html_element_add_class` to add `text-lg`, and `cwist_html_element_set_text`
to set the text inside the tag. `cwist_html_render` returns the HTML as a new
`cwist_sstring`, which the caller releases with `cwist_sstring_destroy`.

**Nested elements.** `cwist_html_element_add_child(parent, child)` attaches an
element to a parent. The example builds three `li` elements, adds them to a
`ul`, and adds the `ul` to a `div`. Rendering the `div` renders the whole tree.
Only the root needs to be destroyed: `cwist_html_element_destroy` destroys the
element and its children, so the example calls it on the `div` and not on the
`ul` or the `li` elements.

**Anchor with attributes.** `cwist_html_element_add_attr(el, key, value)` adds
any attribute. The example adds `href` and `target` to an `a` element.

## Behaviour to know about

* Text and attribute values are HTML-escaped when rendered. Text such as
  `1 < 2 & <b>bold</b>` is written as `1 &lt; 2 &amp; &lt;b&gt;bold&lt;/b&gt;`,
  and a `"` in an attribute value is written as `&quot;`.
* Calling `cwist_html_element_add_class` more than once adds more classes: two
  calls with `a` and `b` produce `class="a b"`.
* Void elements (`br`, `img`, `input`, `meta` and the others listed in the
  header comment of `cwist_html_render`) are written as a start tag only, such as
  `<br>`. Text or children attached to them are not rendered.
