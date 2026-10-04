# Cookie API

*Header:* `<cwist/net/http/cookie.h>`

Helpers for reading the request `Cookie` header and writing `Set-Cookie`
response headers. Parsed cookies are stored in a `cwist_query_map` (see the
[Query & URI API](query.md)).

## Reading cookies

### `cwist_cookie_parse`
```c
void cwist_cookie_parse(cwist_query_map *map, const char *header);
```
Parses a `Cookie` header value of the form `name1=value1; name2=value2` and
stores each pair in `map`, which must come from `cwist_query_map_create()`.

* Leading whitespace before a name and trailing whitespace after a name are
  removed. Whitespace at the end of a value is kept.
* Each value is URL-decoded with `cwist_cookie_decode`.
* Pairs with no `=`, with an empty name, or whose decoded value does not fit in
  4096 bytes are skipped.
* A `NULL` or empty `header`, or a `NULL` `map`, does nothing.

### `cwist_cookie_get`
```c
const char *cwist_cookie_get(cwist_query_map *map, const char *name);
```
Returns the decoded value stored for `name`, or `NULL` if there is none. The
pointer belongs to `map` and is valid until the map is destroyed.

## Writing cookies

### `cwist_cookie_options`
```c
typedef struct cwist_cookie_options {
    const char *path;
    const char *domain;
    int max_age_seconds;   /* < 0 means omit */
    bool http_only;
    bool secure;
    const char *same_site; /* "Strict", "Lax", or "None" */
} cwist_cookie_options;
```
Attributes for `cwist_cookie_set`. A `NULL` `path`, `domain` or `same_site` is
left out, and `http_only` and `secure` are added only when `true`.

`max_age_seconds` is written whenever it is `0` or greater, so a
zero-initialised struct produces `Max-Age=0`, which tells the client to delete
the cookie. Set it to `-1` to omit the attribute. `same_site` is copied as given
and is not checked against the three listed values.

### `cwist_cookie_set`
```c
int cwist_cookie_set(cwist_http_response *res, const char *name, const char *value,
                     const cwist_cookie_options *opts);
```
Adds a `Set-Cookie` header to `res` in the form
`name=<encoded value>; Path=...; Domain=...; Max-Age=...; HttpOnly; Secure; SameSite=...`.
The value is encoded with `cwist_cookie_encode`; the name and the attribute
strings are written as given. A `NULL` `value` is sent as an empty value, and a
`NULL` `opts` sends only `name=value`.

Returns `0` on success and `-1` on failure: `res` or `name` is `NULL`, an
allocation fails, or the header is rejected because it contains a CR or LF
character.

### `cwist_cookie_delete`
```c
int cwist_cookie_delete(cwist_http_response *res, const char *name);
```
Adds a `Set-Cookie: name=; Path=/; Max-Age=0` header, which tells the client to
remove the cookie. The path is fixed to `/`, so a cookie that was set with a
different `path` is not matched by this header. Returns `0` on success and `-1`
on failure.

## Encoding

### `cwist_cookie_encode`
```c
char *cwist_cookie_encode(const char *value);
```
Percent-encodes `value`. The characters `A-Z a-z 0-9 - _ . ~` are kept and every
other byte becomes `%XX` with upper-case hex digits (a space becomes `%20`).
Returns a heap string that the caller releases with `cwist_free()`, or `NULL`
if `value` is `NULL` or allocation fails.

### `cwist_cookie_decode`
```c
int cwist_cookie_decode(const char *in, char *out, size_t out_len);
```
Decodes `in` into the caller's buffer `out` of size `out_len`. `%XX` sequences
(upper or lower case hex) become the byte they name, and `+` becomes a space.
A `%` that is not followed by two hex digits is copied unchanged.

Returns the number of bytes written, not counting the terminating `\0`, or `-1`
if an argument is `NULL`, `out_len` is `0`, or the result does not fit in
`out` together with the terminating `\0`.

## Example

Read a cookie from the request and set one on the response:

```c
static void handle_login(cwist_http_request *req, cwist_http_response *res) {
    cwist_query_map *cookies = cwist_query_map_create();
    cwist_cookie_parse(cookies, cwist_http_header_get(req->headers, "Cookie"));
    const char *theme = cwist_cookie_get(cookies, "theme");  /* NULL if absent */
    (void)theme;
    cwist_query_map_destroy(cookies);

    cwist_cookie_options opts = {
        .path = "/",
        .max_age_seconds = 3600,
        .http_only = true,
        .secure = true,
        .same_site = "Lax",
    };
    cwist_cookie_set(res, "session_id", "abc 123", &opts);
    /* Set-Cookie: session_id=abc%20123; Path=/; Max-Age=3600; HttpOnly; Secure; SameSite=Lax */
}
```

See `tests/test_cookie.c` for the inputs and outputs of each function.
