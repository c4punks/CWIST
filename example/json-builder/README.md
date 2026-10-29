# JSON builder example

A small program that builds JSON strings with the helpers declared in
`include/cwist/core/utils/json_builder.h`. See also the
[JSON API page](../../docs/api/json.md).

| Step | Shows |
|------|-------|
| [`step-1-basic`](step-1-basic/main.c) | a flat object, an object with a nested object, and an array of objects |

## Build

Build the library first, from the repository root:

```sh
make
```

Then build the step:

```sh
make -C example/json-builder/step-1-basic
```

`make examples-check` also builds it.

## Run

```sh
./example/json-builder/step-1-basic/step-1-basic
```

Output:

```
=== JSON Builder Tutorial ===

[Flat object]
{"name":"Alice","age":30,"active":true,"note":null}

[Nested object]
{"status":"ok","code":200,"data":{"id":1,"value":42}}

[Array of objects]
[{"id":1,"label":"item-1"},{"id":2,"label":"item-2"},{"id":3,"label":"item-3"}]

=== Done ===
```

## What each part does

**Flat object.** `cwist_json_builder_create()` makes a builder, then
`cwist_json_begin_object`, a series of `cwist_json_add_string`,
`cwist_json_add_int`, `cwist_json_add_bool` and `cwist_json_add_null` calls, and
`cwist_json_end_object` build the text. `cwist_json_get_raw` returns the
finished string. The pointer belongs to the builder and is invalid after
`cwist_json_builder_destroy`.

The builder inserts the commas between members itself. A string value is
JSON-escaped (a `"` or a newline in the value is written as `\"` or `\n`), and a
`NULL` string value is written as `null`.

**Nested object.** `cwist_json_begin_object` takes no key, so the header has no
helper for an object inside another object. The example writes the nested part
by appending raw text to the builder's buffer:

```c
cwist_sstring_append(jb->buffer, ",\"data\":{\"id\":1,\"value\":42}");
```

Text added this way is not escaped or checked, so it has to be valid JSON
including its own leading comma.

**Array of objects.** `cwist_json_begin_array(jb, NULL)` starts a top-level
array. For each element the example appends `{` and `}` itself and sets
`jb->needs_comma = false` before adding the members, because the builder has no
begin/end helper for an object that is an array element. `cwist_json_end_array`
closes the array.

`cwist_json_begin_array` also accepts a key, which writes `"key":[` inside an
object, and elements that are plain values are added with a `NULL` key:

```c
cwist_json_begin_array(jb, "tags");
cwist_json_add_string(jb, NULL, "a");
cwist_json_add_string(jb, NULL, "b");
cwist_json_end_array(jb);   /* "tags":["a","b"] */
```
