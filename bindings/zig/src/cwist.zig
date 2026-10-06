//! Zig API for CWIST, on top of the C API translated from its headers
//! (`c`, see src/cwist.h).
//!
//! ```zig
//! var app = try cwist.App.init();
//! defer app.deinit();
//! try app.get("/users/:id", {}, struct {
//!     fn handle(_: void, req: cwist.Request, res: cwist.Response) void {
//!         res.setBody(req.param("id") orelse "?") catch {};
//!     }
//! }.handle);
//! try app.listen(8080);
//! ```
//!
//! Lifetimes: a `Request` and a `Response` are views of CWIST's objects for
//! one handler call. Slices they return point into CWIST's memory and must
//! not be kept after the handler returns; copy what has to outlive it.
//!
//! Threads: CWIST runs handlers on its worker threads, several at once, so a
//! handler's context must be safe to use from any thread.
//!
//! Errors: a handler returns nothing; CWIST answers with whatever it set on
//! the response. A panic in a handler aborts the process (Zig does not
//! unwind), so nothing can escape into C.

const std = @import("std");

/// The raw C API, for anything this module does not wrap yet.
pub const c = @import("c");

pub const Error = error{
    /// `cwist_app_create()` failed (out of memory).
    AppCreate,
    /// CWIST rejected a route registration.
    Route,
    /// CWIST rejected a middleware registration.
    Middleware,
    /// CWIST rejected a response header, for example one containing CR or LF.
    Header,
    /// CWIST could not store a response body (out of memory).
    Body,
    /// In-memory dispatch failed: the request was malformed or the response
    /// could not be serialized.
    Dispatch,
    /// The server could not start, for example because the port is in use.
    Listen,
    /// The response was deferred to an `AsyncResponse` and can no longer be modified.
    Deferred,
};

/// HTTP request method.
pub const Method = enum { get, post, put, delete, patch, head, options, connect, unknown };

/// Access-log output format.
pub const LogFormat = enum {
    common,
    combined,
    json,

    fn toC(self: LogFormat) c.cwist_log_format_t {
        return switch (self) {
            .common => c.CWIST_LOG_COMMON,
            .combined => c.CWIST_LOG_COMBINED,
            .json => c.CWIST_LOG_JSON,
        };
    }
};

/// Built-in middleware representation.
pub const BuiltinMiddleware = struct {
    func: c.cwist_middleware_func,
};

/// Built-in middleware factories.
pub const middleware = struct {
    var backends_registered: bool = false;

    /// Request ID middleware (sets X-Request-Id header).
    pub fn requestId(header_name: ?[:0]const u8) BuiltinMiddleware {
        const ptr: [*c]const u8 = if (header_name) |h| h.ptr else null;
        return .{ .func = c.cwist_mw_request_id(ptr) };
    }

    /// Access-log middleware.
    pub fn accessLog(format: LogFormat) BuiltinMiddleware {
        return .{ .func = c.cwist_mw_access_log(format.toC()) };
    }

    /// Fixed-window per-IP rate limiter.
    pub fn rateLimitIp(requests_per_minute: i32) BuiltinMiddleware {
        return .{ .func = c.cwist_mw_rate_limit_ip(requests_per_minute) };
    }

    /// Prometheus metrics collector middleware.
    pub fn metrics() BuiltinMiddleware {
        return .{ .func = c.cwist_mw_metrics() };
    }

    /// CORS middleware (sets permissive headers, handles OPTIONS 204).
    pub fn cors() BuiltinMiddleware {
        return .{ .func = c.cwist_mw_cors() };
    }

    /// JWT bearer-token authentication middleware.
    pub fn jwtAuth(secret: [:0]const u8) Error!BuiltinMiddleware {
        const func = c.cwist_mw_jwt_auth(secret.ptr);
        if (func == null) return error.Middleware;
        return .{ .func = func };
    }

    /// Response compression middleware. Registers backend compression algorithms.
    pub fn compress(min_body_size: usize) BuiltinMiddleware {
        if (!backends_registered) {
            c.cwist_compress_register_backend(c.cwist_compress_backend_gzip());
            c.cwist_compress_register_backend(c.cwist_compress_backend_deflate());
            c.cwist_compress_register_backend(c.cwist_compress_backend_brotli());
            c.cwist_compress_register_backend(c.cwist_compress_backend_zstd());
            backends_registered = true;
        }
        return .{ .func = c.cwist_mw_compress(min_body_size) };
    }
};

/// Token passed to a middleware to proceed to the next middleware or route handler.
pub const Next = struct {
    raw_req: [*c]c.cwist_http_request,
    raw_res: [*c]c.cwist_http_response,
    raw_next: c.cwist_handler_func,
    called: bool = false,

    /// Advances the chain. Only the first call takes effect.
    pub fn call(self: *Next) void {
        if (self.called) return;
        self.called = true;
        if (self.raw_next) |f| {
            f(self.raw_req, self.raw_res);
        }
    }
};

/// Tracks whether in-memory dispatch is currently running on this thread.
threadlocal var in_memory_dispatch: bool = false;

/// Checks a `cwist_error_t` and releases it, as C callers must.
fn consume(err_in: c.cwist_error_t) bool {
    var err = err_in;
    const ok = c.cwist_error_is_ok_extern(&err);
    c.cwist_error_dispose(&err);
    return ok;
}

/// Bytes of a `cwist_sstring`, or "" when it is NULL or empty.
fn sstringBytes(s: [*c]c.cwist_sstring) []const u8 {
    if (s == null) return "";
    const str = s.*;
    if (str.data == null or str.size == 0) return "";
    return str.data[0..str.size];
}

/// A NUL-terminated C string returned by CWIST, or null.
fn cString(p: [*c]const u8) ?[]const u8 {
    if (p == null) return null;
    return std.mem.span(@as([*:0]const u8, @ptrCast(p)));
}

/// The request a handler receives; a view for one handler call.
pub const Request = struct {
    raw: *c.cwist_http_request,

    /// The request method.
    pub fn method(self: Request) Method {
        return switch (self.raw.method) {
            c.CWIST_HTTP_GET => .get,
            c.CWIST_HTTP_POST => .post,
            c.CWIST_HTTP_PUT => .put,
            c.CWIST_HTTP_DELETE => .delete,
            c.CWIST_HTTP_PATCH => .patch,
            c.CWIST_HTTP_HEAD => .head,
            c.CWIST_HTTP_OPTIONS => .options,
            c.CWIST_HTTP_CONNECT => .connect,
            else => .unknown,
        };
    }

    /// The request path without the query string, e.g. "/users/7".
    pub fn path(self: Request) []const u8 {
        return sstringBytes(self.raw.path);
    }

    /// The request body.
    pub fn body(self: Request) []const u8 {
        return sstringBytes(self.raw.body);
    }

    /// The value of a request header (case-insensitive name), if present.
    pub fn header(self: Request, name: [:0]const u8) ?[]const u8 {
        return cString(c.cwist_http_header_get(self.raw.headers, name.ptr));
    }

    /// A path parameter captured by a `:name` segment of the route.
    pub fn param(self: Request, name: [:0]const u8) ?[]const u8 {
        if (self.raw.path_params == null) return null;
        return cString(c.cwist_query_map_get(self.raw.path_params, name.ptr));
    }

    /// A query-string parameter (`?name=value`).
    pub fn query(self: Request, name: [:0]const u8) ?[]const u8 {
        if (self.raw.query_params == null) return null;
        return cString(c.cwist_query_map_get(self.raw.query_params, name.ptr));
    }
};

/// The response a handler fills in; a view for one handler call.
pub const Response = struct {
    raw: *c.cwist_http_response,

    /// Returns true if the response was deferred.
    pub fn isDeferred(self: Response) bool {
        return self.raw.deferred;
    }

    /// Returns the status code, or 0 if deferred.
    pub fn status(self: Response) u16 {
        if (self.isDeferred()) return 0;
        return self.raw.status_code;
    }

    /// Sets the status code, e.g. 201. Ignored if deferred.
    pub fn setStatus(self: Response, code: u16) void {
        if (self.isDeferred()) return;
        self.raw.status_code = code;
    }

    /// Replaces the response body. CWIST copies the bytes.
    pub fn setBody(self: Response, bytes: []const u8) Error!void {
        if (self.isDeferred()) return error.Deferred;
        if (self.raw.body == null) return error.Body;
        if (!consume(c.cwist_sstring_assign_len(self.raw.body, bytes.ptr, bytes.len))) {
            return error.Body;
        }
    }

    /// Adds a response header. CWIST copies both strings and rejects names
    /// or values containing CR or LF.
    pub fn addHeader(self: Response, name: [:0]const u8, value: [:0]const u8) Error!void {
        if (self.isDeferred()) return error.Deferred;
        if (!consume(c.cwist_http_header_add(&self.raw.headers, name.ptr, value.ptr))) {
            return error.Header;
        }
    }
};

/// An independently-allocated HTTP response object owned by the caller.
/// Used for constructing full responses (e.g. for `AsyncResponse.respondWith`).
pub const OwnedResponse = struct {
    raw: *c.cwist_http_response,

    /// Allocates a new empty HTTP response.
    pub fn init() Error!OwnedResponse {
        const ptr = c.cwist_http_response_create();
        if (ptr == null) return error.Body;
        return .{ .raw = ptr };
    }

    /// Destroys the response and frees all associated memory.
    pub fn deinit(self: *OwnedResponse) void {
        c.cwist_http_response_destroy(self.raw);
        self.* = undefined;
    }

    /// Sets the status code.
    pub fn setStatus(self: *OwnedResponse, code: u16) void {
        self.raw.status_code = code;
    }

    /// Replaces the response body.
    pub fn setBody(self: *OwnedResponse, bytes: []const u8) Error!void {
        if (self.raw.body == null) return error.Body;
        if (!consume(c.cwist_sstring_assign_len(self.raw.body, bytes.ptr, bytes.len))) {
            return error.Body;
        }
    }

    /// Adds a response header.
    pub fn addHeader(self: *OwnedResponse, name: [:0]const u8, value: [:0]const u8) Error!void {
        if (!consume(c.cwist_http_header_add(&self.raw.headers, name.ptr, value.ptr))) {
            return error.Header;
        }
    }
};

/// A deferred HTTP response handle obtained inside a route handler.
/// Can be sent to background threads or event loops to be completed asynchronously.
pub const AsyncResponse = struct {
    raw: *c.cwist_async,

    /// Defers the response for the current request.
    /// Returns null if already deferred, during in-memory dispatch, or if allocation fails.
    pub fn @"defer"(req: Request, res: Response) ?AsyncResponse {
        if (in_memory_dispatch) return null;
        const handle = c.cwist_async_defer(req.raw, res.raw);
        if (handle == null) return null;
        _ = c.cwist_async_retain(handle);
        return .{ .raw = handle.? };
    }

    /// Sets a timeout in milliseconds. If still pending upon expiry, answers with 504.
    pub fn setTimeout(self: AsyncResponse, ms: u64) void {
        c.cwist_async_set_timeout(self.raw, ms);
    }

    /// Completes the exchange with a status code, content-type and body.
    /// Returns true if this was the first completion call to win.
    pub fn respond(self: AsyncResponse, status_code: u16, content_type: [:0]const u8, body_bytes: []const u8) bool {
        const ok = c.cwist_async_respond(
            self.raw,
            status_code,
            content_type.ptr,
            body_bytes.ptr,
            body_bytes.len,
        );
        c.cwist_async_release(self.raw);
        return ok;
    }

    /// Completes the exchange with a caller-constructed `OwnedResponse`.
    /// On success CWIST takes ownership of the response.
    pub fn respondWith(self: AsyncResponse, res: *OwnedResponse) bool {
        const ok = c.cwist_async_respond_with(self.raw, res.raw);
        if (!ok) {
            res.deinit();
        }
        res.* = undefined;
        c.cwist_async_release(self.raw);
        return ok;
    }

    /// Completes the exchange with an error status and closes the connection.
    pub fn abort(self: AsyncResponse, status_code: u16) bool {
        const ok = c.cwist_async_abort(self.raw, status_code);
        c.cwist_async_release(self.raw);
        return ok;
    }

    /// Drops the handle without explicit completion, responding with 500.
    pub fn deinit(self: *AsyncResponse) void {
        _ = c.cwist_async_abort(self.raw, c.CWIST_HTTP_INTERNAL_ERROR);
        c.cwist_async_release(self.raw);
        self.* = undefined;
    }
};

const RegisterFn = *const fn (
    [*c]c.cwist_app,
    [*c]const u8,
    c.cwist_handler_ex_func,
    ?*anyopaque,
    c.cwist_handler_ctx_destroy_func,
) callconv(.c) c.cwist_error_t;

/// The C entry point for a route: turns CWIST's user context back into the
/// handler's context and calls it.
fn Trampoline(comptime Context: type, comptime handler: fn (Context, Request, Response) void) type {
    switch (@typeInfo(Context)) {
        .void, .pointer => {},
        else => @compileError("a route context must be a pointer or void, not " ++ @typeName(Context)),
    }
    return struct {
        fn call(
            user_ctx: ?*anyopaque,
            req: [*c]c.cwist_http_request,
            res: [*c]c.cwist_http_response,
        ) callconv(.c) void {
            if (req == null or res == null) return;
            const context: Context = if (Context == void) {} else @ptrCast(@alignCast(user_ctx.?));
            handler(context, .{ .raw = req }, .{ .raw = res });
        }
    };
}

/// The C entry point for extended middleware: wraps next into a `Next` struct and calls handler.
fn MiddlewareTrampoline(comptime Context: type, comptime mw_fn: fn (Context, Request, Response, *Next) void) type {
    switch (@typeInfo(Context)) {
        .void, .pointer => {},
        else => @compileError("a middleware context must be a pointer or void, not " ++ @typeName(Context)),
    }
    return struct {
        fn call(
            req: [*c]c.cwist_http_request,
            res: [*c]c.cwist_http_response,
            next_func: c.cwist_handler_func,
            user_ctx: ?*anyopaque,
        ) callconv(.c) void {
            if (req == null or res == null) return;
            const context: Context = if (Context == void) {} else @ptrCast(@alignCast(user_ctx.?));
            var n: Next = .{
                .raw_req = req,
                .raw_res = res,
                .raw_next = next_func,
            };
            mw_fn(context, .{ .raw = req }, .{ .raw = res }, &n);
        }
    };
}

/// A CWIST application: routes plus the C app object that serves them.
///
/// Route and middleware contexts are borrowed, not owned: each must stay valid until
/// `deinit`.
pub const App = struct {
    raw: *c.cwist_app,

    /// Creates an empty application.
    pub fn init() Error!App {
        const raw = c.cwist_app_create();
        if (raw == null) return error.AppCreate;
        return .{ .raw = raw };
    }

    /// Destroys the C app.
    pub fn deinit(self: *App) void {
        c.cwist_app_destroy(self.raw);
        self.* = undefined;
    }

    /// Registers custom middleware with a borrowed context.
    pub fn useMiddleware(
        self: *App,
        context: anytype,
        comptime mw_fn: fn (@TypeOf(context), Request, Response, *Next) void,
    ) Error!void {
        const Context = @TypeOf(context);
        const user_ctx: ?*anyopaque = if (Context == void) null else @ptrCast(@constCast(context));
        const err = c.cwist_app_use_ex(
            self.raw,
            null,
            &MiddlewareTrampoline(Context, mw_fn).call,
            user_ctx,
            null,
        );
        if (!consume(err)) return error.Middleware;
    }

    /// Registers a built-in middleware from the `cwist.middleware` namespace.
    pub fn useBuiltinMiddleware(self: *App, mw: BuiltinMiddleware) Error!void {
        const err = c.cwist_app_use_ex(self.raw, mw.func, null, null, null);
        if (!consume(err)) return error.Middleware;
    }

    /// Registers a `GET` route. `path` may contain `:name` segments, read in
    /// the handler with `Request.param`. `context` is a pointer (or `{}`) that
    /// CWIST passes back to `handler` on every request; it is borrowed and
    /// must outlive the app. Registering the same method and path again
    /// replaces the previous handler.
    pub fn get(
        self: *App,
        path: [:0]const u8,
        context: anytype,
        comptime handler: fn (@TypeOf(context), Request, Response) void,
    ) Error!void {
        return self.route(c.cwist_app_get_ex, path, context, handler);
    }

    /// Registers a `POST` route; see `get`.
    pub fn post(
        self: *App,
        path: [:0]const u8,
        context: anytype,
        comptime handler: fn (@TypeOf(context), Request, Response) void,
    ) Error!void {
        return self.route(c.cwist_app_post_ex, path, context, handler);
    }

    /// Registers a `PUT` route; see `get`.
    pub fn put(
        self: *App,
        path: [:0]const u8,
        context: anytype,
        comptime handler: fn (@TypeOf(context), Request, Response) void,
    ) Error!void {
        return self.route(c.cwist_app_put_ex, path, context, handler);
    }

    /// Registers a `DELETE` route; see `get`.
    pub fn delete(
        self: *App,
        path: [:0]const u8,
        context: anytype,
        comptime handler: fn (@TypeOf(context), Request, Response) void,
    ) Error!void {
        return self.route(c.cwist_app_delete_ex, path, context, handler);
    }

    /// Registers a `PATCH` route; see `get`.
    pub fn patch(
        self: *App,
        path: [:0]const u8,
        context: anytype,
        comptime handler: fn (@TypeOf(context), Request, Response) void,
    ) Error!void {
        return self.route(c.cwist_app_patch_ex, path, context, handler);
    }

    fn route(
        self: *App,
        register: RegisterFn,
        path: [:0]const u8,
        context: anytype,
        comptime handler: fn (@TypeOf(context), Request, Response) void,
    ) Error!void {
        const Context = @TypeOf(context);
        const user_ctx: ?*anyopaque = if (Context == void) null else @ptrCast(@constCast(context));
        // No destructor: the context is borrowed and stays the caller's.
        const err = register(self.raw, path.ptr, &Trampoline(Context, handler).call, user_ctx, null);
        if (!consume(err)) return error.Route;
    }

    /// Runs one raw HTTP/1.x request through the router and handlers in
    /// memory, with no socket, and returns the serialized response (status
    /// line, headers and body), allocated with `allocator`.
    pub fn dispatch(self: *App, allocator: std.mem.Allocator, request: []const u8) (Error || std.mem.Allocator.Error)![]u8 {
        var out: [*c]u8 = null;
        var out_len: usize = 0;
        in_memory_dispatch = true;
        defer in_memory_dispatch = false;
        const rc = c.cwist_app_dispatch_memory(self.raw, request.ptr, request.len, &out, &out_len);
        defer c.cwist_free(out);
        if (rc != 0 or out == null) return error.Dispatch;
        return allocator.dupe(u8, out[0..out_len]);
    }

    /// Serves this app on `port` (all IPv4 interfaces) and blocks until a
    /// graceful shutdown is requested with `shutdown()` or SIGTERM/SIGINT;
    /// the server then stops accepting, drains and returns.
    pub fn listen(self: *App, port: u16) Error!void {
        const rc = c.cwist_app_listen_ex(self.raw, port, 1, 1);
        // A shutdown leaves the process-wide running flag cleared; reset it
        // so a later listen can serve.
        c.cwist_shutdown_reset();
        if (rc != 0) return error.Listen;
    }
};

/// Requests a graceful shutdown of the running server, as SIGTERM/SIGINT
/// do. Safe from any thread, including a handler; repeated calls are
/// harmless.
pub fn shutdown() void {
    c.cwist_shutdown_request();
}

// --- Tests: the API against the real libcwist, through in-memory dispatch.

const testing = std.testing;

fn testRequest(comptime method_line: []const u8, comptime extra: []const u8, comptime body_text: []const u8) []const u8 {
    return std.fmt.comptimePrint(
        "{s} HTTP/1.1\r\nHost: localhost\r\n{s}Content-Length: {d}\r\n\r\n{s}",
        .{ method_line, extra, body_text.len, body_text },
    );
}

fn bodyOf(response: []const u8) []const u8 {
    const split = std.mem.indexOf(u8, response, "\r\n\r\n") orelse return "";
    return response[split + 4 ..];
}

fn echoUser(_: void, req: Request, res: Response) void {
    var buf: [128]u8 = undefined;
    const text = std.fmt.bufPrint(&buf, "{s} {s} as {s} from {s}", .{
        @tagName(req.method()),
        req.param("id") orelse "?",
        req.query("fmt") orelse "plain",
        req.header("x-client") orelse "none",
    }) catch "too long";
    res.setStatus(200);
    res.addHeader("X-Route", "users") catch {};
    res.setBody(text) catch {};
}

fn echoBody(_: void, req: Request, res: Response) void {
    res.setStatus(201);
    res.setBody(req.body()) catch {};
}

test "routes read the request and write the response" {
    var app = try App.init();
    defer app.deinit();
    try app.get("/users/:id", {}, echoUser);
    try app.post("/echo", {}, echoBody);

    const users = try app.dispatch(testing.allocator, testRequest("GET /users/42?fmt=json", "X-Client: zig\r\n", ""));
    defer testing.allocator.free(users);
    try testing.expect(std.mem.startsWith(u8, users, "HTTP/1.1 200"));
    try testing.expect(std.mem.indexOf(u8, users, "X-Route: users\r\n") != null);
    try testing.expectEqualStrings("get 42 as json from zig", bodyOf(users));

    const echo = try app.dispatch(testing.allocator, testRequest("POST /echo", "", "payload"));
    defer testing.allocator.free(echo);
    try testing.expect(std.mem.startsWith(u8, echo, "HTTP/1.1 201"));
    try testing.expectEqualStrings("payload", bodyOf(echo));

    const missing = try app.dispatch(testing.allocator, testRequest("GET /nope", "", ""));
    defer testing.allocator.free(missing);
    try testing.expect(std.mem.startsWith(u8, missing, "HTTP/1.1 404"));
}

const Counter = struct {
    hits: std.atomic.Value(u32) = .init(0),
};

fn countHit(counter: *Counter, _: Request, res: Response) void {
    const n = counter.hits.fetchAdd(1, .monotonic) + 1;
    var buf: [16]u8 = undefined;
    res.setBody(std.fmt.bufPrint(&buf, "{d}", .{n}) catch "?") catch {};
}

test "a route context is passed back to its handler" {
    var counter: Counter = .{};
    var app = try App.init();
    defer app.deinit();
    try app.get("/count", &counter, countHit);

    for (1..4) |i| {
        const out = try app.dispatch(testing.allocator, testRequest("GET /count", "", ""));
        defer testing.allocator.free(out);
        var buf: [16]u8 = undefined;
        try testing.expectEqualStrings(try std.fmt.bufPrint(&buf, "{d}", .{i}), bodyOf(out));
    }
    try testing.expectEqual(@as(u32, 3), counter.hits.load(.monotonic));
}

fn badHeader(_: void, _: Request, res: Response) void {
    // CR/LF would split the response; CWIST refuses it and nothing is added.
    if (res.addHeader("X-Bad", "a\r\nInjected: yes")) |_| {
        res.setBody("added") catch {};
    } else |err| {
        res.setBody(@errorName(err)) catch {};
    }
}

test "a header with CR or LF is rejected" {
    var app = try App.init();
    defer app.deinit();
    try app.get("/bad", {}, badHeader);
    const out = try app.dispatch(testing.allocator, testRequest("GET /bad", "", ""));
    defer testing.allocator.free(out);
    try testing.expectEqualStrings("Header", bodyOf(out));
    try testing.expect(std.mem.indexOf(u8, out, "Injected") == null);
}

test "a malformed request is a dispatch error" {
    var app = try App.init();
    defer app.deinit();
    try testing.expectError(error.Dispatch, app.dispatch(testing.allocator, "this is not http"));
}

fn testMiddleware(_: void, req: Request, res: Response, next_tok: *Next) void {
    _ = req;
    res.addHeader("X-Before", "zig-client") catch {};
    next_tok.call();
    res.setStatus(202);
    res.addHeader("X-After", "done") catch {};
}

fn testRouteHandler(_: void, _: Request, res: Response) void {
    res.setStatus(200);
    res.setBody("handler body") catch {};
}

test "custom middleware runs before and after next" {
    var app = try App.init();
    defer app.deinit();

    try app.useMiddleware({}, testMiddleware);
    try app.get("/mw", {}, testRouteHandler);

    const out = try app.dispatch(testing.allocator, testRequest("GET /mw", "X-Client: test\r\n", ""));
    defer testing.allocator.free(out);

    try testing.expect(std.mem.startsWith(u8, out, "HTTP/1.1 202"));
    try testing.expect(std.mem.indexOf(u8, out, "X-Before: zig-client\r\n") != null);
    try testing.expect(std.mem.indexOf(u8, out, "X-After: done\r\n") != null);
    try testing.expectEqualStrings("handler body", bodyOf(out));
}

fn shortCircuitMiddleware(_: void, req: Request, res: Response, next_tok: *Next) void {
    if (std.mem.eql(u8, req.path(), "/blocked")) {
        res.setStatus(403);
        res.setBody("forbidden") catch {};
        return;
    }
    next_tok.call();
}

test "middleware can short-circuit the chain" {
    var app = try App.init();
    defer app.deinit();

    try app.useMiddleware({}, shortCircuitMiddleware);
    try app.get("/blocked", {}, testRouteHandler);

    const out = try app.dispatch(testing.allocator, testRequest("GET /blocked", "", ""));
    defer testing.allocator.free(out);

    try testing.expect(std.mem.startsWith(u8, out, "HTTP/1.1 403"));
    try testing.expectEqualStrings("forbidden", bodyOf(out));
}

test "builtin middleware cors sets access-control headers on routes" {
    var app = try App.init();
    defer app.deinit();

    try app.useBuiltinMiddleware(middleware.cors());
    try app.get("/cors-test", {}, testRouteHandler);

    const res_get = try app.dispatch(testing.allocator, testRequest("GET /cors-test", "Origin: http://example.com\r\n", ""));
    defer testing.allocator.free(res_get);
    try testing.expect(std.mem.startsWith(u8, res_get, "HTTP/1.1 200"));
    try testing.expect(std.mem.indexOf(u8, res_get, "Access-Control-Allow-Origin") != null);
    try testing.expectEqualStrings("handler body", bodyOf(res_get));
}

test "builtin middleware compress compresses response body" {
    var app = try App.init();
    defer app.deinit();

    try app.useBuiltinMiddleware(middleware.compress(32));
    const LargeHandler = struct {
        fn handle(_: void, _: Request, res: Response) void {
            res.setStatus(200);
            res.setBody("01234567890123456789012345678901234567890123456789") catch {};
        }
    };
    try app.get("/large", {}, LargeHandler.handle);

    const out = try app.dispatch(testing.allocator, testRequest("GET /large", "Accept-Encoding: gzip\r\n", ""));
    defer testing.allocator.free(out);
    try testing.expect(std.mem.startsWith(u8, out, "HTTP/1.1 200"));
    try testing.expect(std.mem.indexOf(u8, out, "Content-Encoding: gzip\r\n") != null);
}

test "OwnedResponse creates and sets fields" {
    var owned = try OwnedResponse.init();
    defer owned.deinit();

    owned.setStatus(201);
    try owned.addHeader("X-Custom", "ZigOwned");
    try owned.setBody("Hello from OwnedResponse");

    try testing.expectEqual(@as(u16, 201), owned.raw.status_code);
    try testing.expectEqualStrings("Hello from OwnedResponse", sstringBytes(owned.raw.body));
}

fn deferAttemptHandler(_: void, req: Request, res: Response) void {
    // In dispatch mode, defer returns null and response remains synchronous
    const async_handle = AsyncResponse.@"defer"(req, res);
    if (async_handle == null) {
        res.setStatus(200);
        res.setBody("in-memory dispatch refused defer") catch {};
    }
}

test "AsyncResponse defer behavior in dispatch" {
    var app = try App.init();
    defer app.deinit();
    try app.get("/async", {}, deferAttemptHandler);

    const out = try app.dispatch(testing.allocator, testRequest("GET /async", "", ""));
    defer testing.allocator.free(out);
    try testing.expect(std.mem.startsWith(u8, out, "HTTP/1.1 200"));
    try testing.expectEqualStrings("in-memory dispatch refused defer", bodyOf(out));
}
