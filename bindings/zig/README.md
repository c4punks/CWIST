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

## Safe-wrapper surface

The `src/cwist.zig` safe layer covers the following types and functions.
Everything else is accessible through the raw C translation at `cwist.c`
(see "Raw access" below).

| Symbol | Description |
|---|---|
| `Error` | Error set mirroring `CWIST_ERROR_*` |
| `Method` | HTTP method enum |
| `LogFormat` | Access-log format enum |
| `middleware.requestId` | Request-ID header injection |
| `middleware.accessLog` | Structured access logging |
| `middleware.rateLimitIp` | Per-IP request rate limiting |
| `middleware.metrics` | Prometheus metrics endpoint |
| `middleware.cors` | CORS preflight and headers |
| `middleware.jwtAuth` | HMAC-SHA256 JWT bearer authentication |
| `middleware.compress` | Response compression |
| `Request` | Borrowed view of one HTTP request |
| `Response` | Borrowed view of one HTTP response |
| `OwnedResponse` | Heap-allocated response for out-of-handler use |
| `AsyncResponse` | Deferred response across threads |
| `App` | Server lifecycle, route and middleware registration |
| `shutdown` | Graceful shutdown from any thread |

## Raw-only (not yet wrapped)

The features below are available through the raw C translation (`cwist.c`)
but have no Zig safe wrapper. A future PR may add one for each.

| CWIST feature | C header |
|---|---|
| Database / ORM (`cwist_db_*`, `cwist_orm_*`) | `cwist/sys/app/db.h`, `cwist/core/db/orm.h` |
| Scheduler (`cwist_scheduler_*`) | `cwist/sys/app/scheduler.h` |
| JWT asymmetric algorithms (`cwist_jwt_*`) | `cwist/core/security/jwt.h` |
| Redis client (`cwist_redis_*`) | `cwist/sys/app/redis.h` |
| SipHash (`cwist_siphash*`) | `cwist/core/crypto/siphash.h` |
| Short string (`cwist_sstring_*`) | `cwist/core/util/sstring.h` |
| Arena allocator (`cwist_arena_*`) | `cwist/core/mem/arena.h` |
| WebSocket (`cwist_websocket_*`) | `cwist/net/ws/websocket.h` |
| TLS socket configuration (`cwist_app_set_tls_groups`, `cwist_app_ws_opt`) | `cwist/sys/app/app.h` |
| HTTP/3 and WebTransport | `cwist/net/http3/http3.h` |
| gRPC | `cwist/net/grpc/grpc.h` |
| Multiport listen (`cwist_app_multiport`) | `cwist/sys/app/app.h` |

## Raw access

Use `cwist.c` to call any CWIST function directly from Zig:

```zig
const cwist = @import("cwist");

fn handler(ctx: void, req: cwist.Request, res: cwist.Response) void {
    _ = ctx;
    _ = req;

    // Raw C call for a feature without a Zig wrapper.
    const log = cwist.c.cwist_logger_create("myapp");
    cwist.c.cwist_logger_info(log, "request received");
    cwist.c.cwist_logger_destroy(log);

    res.setStatus(200) catch {};
    res.setBody("ok") catch {};
}
```

All pointers from the raw C API follow CWIST's ownership rules as documented
in the C headers; Zig does not manage their lifetimes automatically.

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
