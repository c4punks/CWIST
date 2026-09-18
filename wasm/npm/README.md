# cwist-wasm

First-party JS wrapper over CWIST's WASM `dispatch_memory` path (issue #93
Phase 2). You compile your CWIST app (routes + handlers) to WebAssembly with
Emscripten; this package hides the Emscripten glue and gives you a
fetch-shaped API.

## Install

```sh
npm install cwist-wasm
```

## Build your app module

Write a normal CWIST app, register routes, then define the standard entry
points once:

```c
#include <cwist/sys/app/app.h>
#include <cwist/wasm/wasm_entry.h>

static cwist_app *g_app;

static void hello(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "hello from wasm");
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
}

int main(void) {
    g_app = cwist_app_create();
    cwist_app_get(g_app, "/hello", hello);
    CWIST_WASM_DEFINE_ENTRY(g_app);
    return 0;
}
```

Compile against `libcwist_wasm.a` (built with `make wasm` in the CWIST
repo) with the entry points and runtime methods exported:

```sh
emcc -O2 -std=c17 -I$cwist/include -I$cwist/lib -o app.js my_app.c \
    $cwist/libcwist_wasm.a \
    -sEXPORTED_FUNCTIONS=_cwist_wasm_dispatch,_cwist_wasm_dispose,_malloc,_free \
    -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32
```

## Use from JS

```js
const { createCwist } = require('cwist-wasm');
// In Node, load app.js first (e.g. require('./app.js')) so `Module` exists.
const handle = createCwist(Module);

const res = handle({ method: 'GET', path: '/hello' });
console.log(res.status);                  // 200
console.log(res.headers['Content-Type']); // text/plain
console.log(new TextDecoder().decode(res.body)); // hello from wasm
```

Request `body` may be a string or a `Uint8Array`. The response `body` is a
copy, so it stays valid across subsequent calls even if the WASM heap is
reused.

## Scope

This wrapper is intentionally thin: routing, middleware, validation, and
rendering all stay in your C handlers. It only serializes requests into
HTTP/1.1 bytes, calls `cwist_app_dispatch_memory()` inside WASM, and parses
the serialized response back into JS objects. Streaming and session
persistence are not handled here; see `docs/api/wasm.md` in the CWIST repo
for the current caveats.
