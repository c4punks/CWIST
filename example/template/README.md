# template examples

Rendering text templates with the CWIST template engine declared in
`include/cwist/core/template/template.h`. A single `cwist_template_render`
call takes a template string and a cJSON context and returns the filled
result as a `cwist_sstring`.

| Step | Shows |
|------|-------|
| [`step-1-render`](step-1-render/main.c) | variable substitution with `{{ name }}`, conditional blocks with `{% if %}`, and loops over arrays with `{% for %}` |

## Build

Build the library first, from the repository root:

```sh
make
```

Then build the example (it ships its own Makefile):

```sh
make -C example/template/step-1-render
```

`make examples-check` also builds it.

## Run

```sh
./example/template/step-1-render/step-1-render
```

The program renders three templates against three cJSON contexts:

1. **Variable substitution** — `{{ name }}` and `{{ version }}` are
   replaced by the matching context fields.
2. **Conditional block** — `{% if logged_in %}...{% endif %}` is emitted
   when the field is true; the `{% if guest %}` block is skipped because
   the context has no `guest` field.
3. **Loop over array** — `{% for fruit in fruits %}` repeats its body
   for each element and resolves `{{ fruit.name }}` inside the loop.

Example output:

```
=== Template Rendering Tutorial ===

[Variable substitution]
Output: Hello, CWIST! This is version 1.

[Conditional block]
Output: Welcome back, Alice!

[Loop over array]
Output:
Fruit list:
  - Apple
  - Banana
  - Cherry

=== Done ===
```

## Clean

```sh
make -C example/template/step-1-render clean
```
