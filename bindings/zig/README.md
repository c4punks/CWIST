# CWIST Zig bindings

Zig API for CWIST (issue #36), next to the Rust bindings in `bindings/rust/`.

* The C API is translated from the installed CWIST headers at build time
  (`b.addTranslateC`, see `src/cwist.h`) and available as `cwist.c`.
* `src/cwist.zig` adds a Zig layer on top:
  * `App` with `get`/`post`/`put`/`delete`/`patch` routes
  * Custom middleware (`useMiddleware`) and built-in middleware (`useBuiltinMiddleware`)
  * `Request`, `Response`, and `OwnedResponse`
  * Deferred async responses (`AsyncResponse.defer`, `respond`, `respondWith`, `abort`, `setTimeout`)
  * In-memory `dispatch`, `listen` and `shutdown`

## Zig version

Pinned to **Zig 0.17.0** (`minimum_zig_version` in `build.zig.zon`, and the
exact release in CI). Zig is pre-1.0 and changes its build and language APIs
between releases, so the pin moves only in a dedicated PR.

## Build

CWIST must be built and installed first; libcwist is found through
`pkg-config --static cwist`:

```bash
make && make install PREFIX=$HOME/.local
export PKG_CONFIG_PATH=$HOME/.local/lib/pkgconfig
cd bindings/zig && zig build test
```

`build.zig` links libcwist and its bundled dependencies statically from the
`cwist.pc` link directories; `-lstdc++` maps to Zig's own libc++. On macOS,
add Homebrew's curl to `PKG_CONFIG_PATH`
(`/opt/homebrew/opt/curl/lib/pkgconfig`), as for the Rust bindings.

## Use from another package

```zig
// build.zig.zon
.dependencies = .{ .cwist = .{ .path = "path/to/CWIST/bindings/zig" } },

// build.zig
const cwist = b.dependency("cwist", .{ .target = target, .optimize = optimize });
exe.root_module.addImport("cwist", cwist.module("cwist"));
```

See `example/zig-hello/` for a complete server.

## Handlers, lifetimes and threads

* A handler is `fn (Context, cwist.Request, cwist.Response) void`. `Context`
  is a pointer, or `void` with `{}` at registration; CWIST hands it back on
  every request. It is borrowed: it must outlive the app.
* `Request` and `Response` are views of CWIST's objects for one handler call.
  Slices from `Request` point into CWIST's memory and must not be kept after
  the handler returns.
* Custom middleware signature is `fn (Context, cwist.Request, cwist.Response, *cwist.Next) void`.
  Calling `next.call()` advances the chain to the next middleware or final handler.
* `AsyncResponse.defer(req, res)` hands response ownership to an asynchronous handle
  that can be completed across threads with `respond`, `respondWith`, or `abort`.
* CWIST runs handlers on several worker threads at once, so a context must
  be safe to use from any thread.
* A handler returns nothing, so no error crosses into C, and a panic aborts
  the process (Zig does not unwind).
* `listen` serves in the calling process on the reactor server, with no
  forked workers, and returns after a graceful shutdown (`cwist.shutdown()`,
  SIGTERM or SIGINT) with every handler thread joined.

Not wrapped yet: TLS socket configuration, WebSocket wrappers. The raw API in
`cwist.c` covers them in the meantime.
