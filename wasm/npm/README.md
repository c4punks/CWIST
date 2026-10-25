# cwist-wasm

First-party JS API over a CWIST app compiled to WebAssembly as a
`cwist-guest` WASI component (`wit/cwist.wit` in the CWIST repo) and
transpiled to JS with [jco](https://github.com/bytecodealliance/jco). You
write a normal CWIST app; this package turns its dispatch export into a
fetch-shaped call.

## Install

```sh
npm install cwist-wasm
```

## Build your app as a component

Register routes in `main()` and define the component exports once:

```c
#include <cwist/sys/app/app.h>
#include <cwist/wasm/wasm_component.h>

#include "cwist_guest.h" /* wit-bindgen c wit/ --world cwist-guest */

static cwist_app *g_app;

static void hello(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "hello from wasm");
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
}

CWIST_WASM_COMPONENT_DEFINE_GUEST(g_app)

int main(void) {
    g_app = cwist_app_create();
    cwist_app_get(g_app, "/hello", hello);
    return 0;
}
```

Compile it for `wasm32-wasip2` with wasi-sdk against the CWIST WASI archive,
embed the world with `wasm-tools component embed`, and transpile with
`jco transpile`. `docs/api/wasm-component.md` in the CWIST repo has the full
recipe; `make component-smoke` and `make wasm-sw-example` are working
examples.

## Use from JS

```js
const { createCwistFromComponent } = require('cwist-wasm');
const component = await import('./out/app.component.js'); // jco output

component.run.run(); // main(): registers the routes
const handle = createCwistFromComponent(component.guest);

const res = handle({ method: 'GET', path: '/hello' });
console.log(res.status);                  // 200
console.log(res.headers['Content-Type']); // text/plain
console.log(new TextDecoder().decode(res.body)); // hello from wasm
```

Request `body` may be a string or a `Uint8Array`; the response `body` is a
copy, safe to keep across calls. `handle.useSession(secret)` pins the session
signing secret before the first session-bearing dispatch (`null` lets CWIST
generate a per-instance one).

Against a 0.2 (`wasm32-wasip2`) guest `handle()` is synchronous. jco lowers
the exports of a 0.3 (`wasm32-wasip3`) guest to async functions, so against
one `handle()` returns a Promise.

The adapter is environment-neutral JS and bundles for browsers as-is. In a
service worker, transpile with `jco transpile --instantiation async` (a
service worker may not use top-level await) and instantiate on first use;
`example/wasm-service-worker` in the CWIST repo does exactly that.

`require('cwist-wasm/component')` resolves to the same adapter.

## Streaming (0.3 guests, JSPI)

A guest built for the `cwist-guest-stream` world (`wasm32-wasip3` only)
streams a response through its `c4punks:cwist/host` import instead of
returning it. The import is async, so running such a guest needs JSPI, which
is still behind a flag: `node --experimental-wasm-jspi`, and the equivalent
browser flag. `createStreamHost()` returns the import; wire it in when you
instantiate or bundle the component, and give the same object to the adapter:

```js
const { createCwistFromComponent, createStreamHost } = require('cwist-wasm');

const host = createStreamHost(); // { sendChunk }: the c4punks:cwist/host import
// ...instantiate or bundle the transpiled guest with `host` as that import...
const handle = createCwistFromComponent({
  dispatch: guest.dispatch,
  useSession: guest.useSession,
  dispatchStream: guestStream.dispatchStream,
  streamHost: host,
});

await handle.stream({ method: 'GET', path: '/events' }, async (chunk) => {
  // chunk is a Uint8Array: the serialized head first, then body slices
  await writer.write(chunk);
});
```

The guest waits for each callback, so a callback that returns a Promise
applies backpressure. If the callback throws or rejects, the dispatch stops
and `stream()` rejects with that error. Streaming dispatches on one handle
run one at a time.

## Scope

This package is intentionally thin: routing, middleware, validation and
rendering all stay in your C handlers. It serializes requests into HTTP/1.1
bytes, calls the component's dispatch export (`cwist_app_dispatch_memory()`
inside the guest), and parses the serialized response back into JS objects.

## Migrating from the Emscripten wrapper

Up to CWIST v4, the package's main export was `createCwist(Module)` over an
Emscripten module built against `libcwist_wasm.a`. That build is gone (issue
#203): rebuild the app as a component as above, replace
`CWIST_WASM_DEFINE_ENTRY(g_app)` with `CWIST_WASM_COMPONENT_DEFINE_GUEST(g_app)`
at file scope, and call `createCwistFromComponent(component.guest)` instead of
`createCwist(Module)`. The handle has the same shape.
