use crate::http::{consume, Request, Response};
use crate::Error;
use cwist_sys as sys;
use std::cell::Cell;
use std::ffi::{c_void, CString};
use std::os::raw::c_char;
use std::panic::{self, AssertUnwindSafe};
use std::ptr::{self, NonNull};
use std::slice;

type Handler = dyn Fn(&Request<'_>, &mut Response<'_>) + Send + Sync + 'static;

/// What CWIST stores as a route's `user_ctx`: one boxed handler.
struct RouteCtx {
    handler: Box<Handler>,
}

type Middleware = dyn Fn(&Request<'_>, &mut Response<'_>, &dyn Fn()) + Send + Sync + 'static;

/// What CWIST stores as an extended middleware's `user_ctx`: one boxed middleware.
struct MiddlewareCtx {
    middleware: Box<Middleware>,
}

type RegisterFn = unsafe extern "C" fn(
    *mut sys::cwist_app,
    *const c_char,
    sys::cwist_handler_ex_func,
    *mut c_void,
    sys::cwist_handler_ctx_destroy_func,
) -> sys::cwist_error_t;

/// A CWIST application: routes plus the C app object that serves them.
///
/// Dropping the `App` destroys the C app, which releases every registered
/// handler exactly once.
///
/// `App` is neither `Send` nor `Sync`: it is configured and dispatched from
/// the thread that owns it. Handlers themselves may run on CWIST worker
/// threads, which is why they must be `Send + Sync`.
pub struct App {
    raw: NonNull<sys::cwist_app>,
}

impl App {
    /// Creates an empty application.
    pub fn new() -> Result<App, Error> {
        // SAFETY: no preconditions; NULL means allocation failed.
        let raw = unsafe { sys::cwist_app_create() };
        NonNull::new(raw).map(|raw| App { raw }).ok_or(Error::AppCreate)
    }

    /// Registers a `GET` route. `path` may contain `:name` segments, read in
    /// the handler with [`Request::param`]. Registering the same method and
    /// path again replaces the previous handler, which is then dropped.
    pub fn get<F>(&mut self, path: &str, handler: F) -> Result<(), Error>
    where
        F: Fn(&Request<'_>, &mut Response<'_>) + Send + Sync + 'static,
    {
        self.route(sys::cwist_app_get_ex, path, Box::new(handler))
    }

    /// Registers a `POST` route; see [`App::get`].
    pub fn post<F>(&mut self, path: &str, handler: F) -> Result<(), Error>
    where
        F: Fn(&Request<'_>, &mut Response<'_>) + Send + Sync + 'static,
    {
        self.route(sys::cwist_app_post_ex, path, Box::new(handler))
    }

    /// Registers a `PUT` route; see [`App::get`].
    pub fn put<F>(&mut self, path: &str, handler: F) -> Result<(), Error>
    where
        F: Fn(&Request<'_>, &mut Response<'_>) + Send + Sync + 'static,
    {
        self.route(sys::cwist_app_put_ex, path, Box::new(handler))
    }

    /// Registers a `DELETE` route; see [`App::get`].
    pub fn delete<F>(&mut self, path: &str, handler: F) -> Result<(), Error>
    where
        F: Fn(&Request<'_>, &mut Response<'_>) + Send + Sync + 'static,
    {
        self.route(sys::cwist_app_delete_ex, path, Box::new(handler))
    }

    /// Registers a `PATCH` route; see [`App::get`].
    pub fn patch<F>(&mut self, path: &str, handler: F) -> Result<(), Error>
    where
        F: Fn(&Request<'_>, &mut Response<'_>) + Send + Sync + 'static,
    {
        self.route(sys::cwist_app_patch_ex, path, Box::new(handler))
    }

    /// Appends a middleware to the application chain.
    ///
    /// Middleware runs in registration order. Each middleware receives the
    /// request, a mutable response, and a `next` closure that advances the
    /// chain. Calling `next` invokes the next middleware or, at the end of the
    /// chain, the matching route handler. A middleware may choose not to call
    /// `next` to short-circuit the request. Only the first call to `next`
    /// runs the rest of the chain; later calls do nothing. The response can
    /// be changed both before and after `next`.
    ///
    /// The middleware is dropped exactly once, when the app is destroyed, or
    /// before this returns [`Error::Middleware`] if CWIST rejects it.
    ///
    /// # Panics
    ///
    /// As with route handlers, a panic in a middleware is caught at the C
    /// boundary and the response becomes a `500 Internal Server Error`. The
    /// chain does not continue past a panicking middleware; if the panic
    /// happens after `next`, the rest of the chain has already run and its
    /// response is replaced.
    pub fn use_middleware<F>(&mut self, middleware: F) -> Result<(), Error>
    where
        F: Fn(&Request<'_>, &mut Response<'_>, &dyn Fn()) + Send + Sync + 'static,
    {
        let ctx = Box::into_raw(Box::new(MiddlewareCtx { middleware: Box::new(middleware) }))
            .cast::<c_void>();
        // SAFETY: the app is live; ctx is a valid MiddlewareCtx whose ownership
        // passes to CWIST on every outcome, including failure (the destructor
        // then runs before this call returns), so Rust must not touch ctx again.
        let err = unsafe {
            sys::cwist_app_use_ex(
                self.raw.as_ptr(),
                None,
                Some(middleware_trampoline_ex),
                ctx,
                Some(drop_middleware_ctx),
            )
        };
        if consume(err) {
            Ok(())
        } else {
            Err(Error::Middleware)
        }
    }

    /// Appends a built-in CWIST middleware factory result to the chain.
    ///
    /// This is the lower-level sibling of [`App::use_middleware`]: it takes a
    /// C function pointer returned by one of the `cwist::middleware` factories
    /// and appends it directly, with no Rust closure or allocation overhead.
    pub fn use_builtin_middleware(&mut self, mw: sys::cwist_middleware_func) {
        // SAFETY: the app is live; mw is a valid middleware function pointer.
        unsafe { sys::cwist_app_use(self.raw.as_ptr(), mw) };
    }

    fn route(&mut self, register: RegisterFn, path: &str, handler: Box<Handler>) -> Result<(), Error> {
        // Checked before the handler is handed over: on this error it is
        // still ours and is simply dropped here.
        let c_path = CString::new(path).map_err(|_| Error::InteriorNul("route path"))?;
        let ctx = Box::into_raw(Box::new(RouteCtx { handler })).cast::<c_void>();
        // SAFETY: the app is live; CWIST copies the path; ctx is a valid
        // RouteCtx whose ownership passes to CWIST on every outcome, including
        // failure (the destructor then runs before this call returns), so
        // Rust must not touch ctx again.
        let err = unsafe {
            register(
                self.raw.as_ptr(),
                c_path.as_ptr(),
                Some(route_trampoline),
                ctx,
                Some(drop_route_ctx),
            )
        };
        if consume(err) {
            Ok(())
        } else {
            Err(Error::Route { path: path.to_owned() })
        }
    }

    /// Runs one raw HTTP/1.x request through the router, middleware and
    /// handlers in memory, with no socket, and returns the serialized
    /// response (status line, headers and body).
    pub fn dispatch(&self, request: &[u8]) -> Result<Vec<u8>, Error> {
        let mut out: *mut c_char = ptr::null_mut();
        let mut out_len: usize = 0;
        // SAFETY: the app is live; request is read only for the call; CWIST
        // allocates `out`, which is released with cwist_free below.
        let rc = unsafe {
            sys::cwist_app_dispatch_memory(
                self.raw.as_ptr(),
                request.as_ptr().cast(),
                request.len(),
                &mut out,
                &mut out_len,
            )
        };
        if rc != 0 || out.is_null() {
            // SAFETY: cwist_free accepts NULL.
            unsafe { sys::cwist_free(out.cast()) };
            return Err(Error::Dispatch);
        }
        // SAFETY: CWIST returned out_len bytes at out; they are copied before
        // the buffer is freed.
        let response = unsafe { slice::from_raw_parts(out as *const u8, out_len) }.to_vec();
        // SAFETY: out came from CWIST's allocator and is not used afterwards.
        unsafe { sys::cwist_free(out.cast()) };
        Ok(response)
    }
}

impl App {
    /// Serves this app on `port` (all IPv4 interfaces) and blocks until a
    /// graceful shutdown is requested with [`shutdown`](crate::shutdown) or
    /// SIGTERM/SIGINT. The server then stops accepting, drains and returns,
    /// and the app is destroyed with its handlers.
    ///
    /// It serves in the calling process on the reactor server, ignoring
    /// `CWIST_WORKERS` and `CWIST_C1M_MODE`: nothing forks, and every handler
    /// thread has been joined when `listen` returns. One server runs per
    /// process; a concurrent second call returns [`Error::AlreadyListening`]
    /// and drops its app. Startup fills unset `CWIST_*` variables with
    /// `setenv`, so other threads must not touch the environment meanwhile.
    /// Returns [`Error::Listen`] if the server cannot start, for example
    /// because the port is in use.
    pub fn listen(self, port: u16) -> Result<(), Error> {
        let _guard = crate::server::ListenGuard::acquire()?;
        // SAFETY: the app is live and owned by self. With one worker and the
        // reactor server every handler thread is joined before this returns,
        // so no handler runs after the app is dropped below.
        let rc = unsafe { sys::cwist_app_listen_ex(self.raw.as_ptr(), port.into(), 1, 1) };
        // A shutdown leaves the process-wide running flag cleared; reset it
        // so a later listen can serve.
        // SAFETY: the server has stopped and the guard is still held.
        unsafe { sys::cwist_shutdown_reset() };
        drop(self);
        if rc == 0 {
            Ok(())
        } else {
            Err(Error::Listen { port })
        }
    }
}

impl Drop for App {
    fn drop(&mut self) {
        // SAFETY: the app is live and owned by self; destroying it runs each
        // route destructor (drop_route_ctx) exactly once.
        unsafe { sys::cwist_app_destroy(self.raw.as_ptr()) };
    }
}

/// C entry point for every route: calls the boxed handler, never letting a
/// panic unwind into C.
unsafe extern "C" fn route_trampoline(
    user_ctx: *mut c_void,
    req: *mut sys::cwist_http_request,
    res: *mut sys::cwist_http_response,
) {
    let (Some(req), Some(res)) = (NonNull::new(req), NonNull::new(res)) else {
        return;
    };
    let outcome = panic::catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: user_ctx is the RouteCtx registered with this route, alive
        // until its destructor runs, which CWIST never does during a call.
        let ctx = unsafe { &*(user_ctx as *const RouteCtx) };
        // SAFETY: CWIST owns req/res for the duration of this call and does
        // not touch them while the handler runs.
        let request = unsafe { Request::from_raw(req) };
        let mut response = unsafe { Response::from_raw(res) };
        (ctx.handler)(&request, &mut response);
    }));
    if let Err(payload) = outcome {
        // As in drop_route_ctx, the payload's Drop can panic again. Do not
        // run it outside catch_unwind in this extern "C" callback.
        std::mem::forget(payload);
        // SAFETY: as above; only res is touched. Failures are ignored: this
        // is already the error path and must not panic.
        let mut response = unsafe { Response::from_raw(res) };
        response.set_status(500);
        let _ = response.set_body("Internal Server Error");
    }
}

/// C destructor for a route context: drops the boxed handler. A panic in
/// the handler's own Drop is contained.
unsafe extern "C" fn drop_route_ctx(user_ctx: *mut c_void) {
    if user_ctx.is_null() {
        return;
    }
    let result = panic::catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: user_ctx came from Box::into_raw in App::route, and CWIST
        // calls this destructor exactly once per context.
        drop(unsafe { Box::from_raw(user_ctx as *mut RouteCtx) });
    }));
    // The payload's own Drop could panic again; leak it instead.
    if let Err(payload) = result {
        std::mem::forget(payload);
    }
}

/// C entry point for every extended middleware: calls the boxed middleware,
/// never letting a panic unwind into C.
unsafe extern "C" fn middleware_trampoline_ex(
    req: *mut sys::cwist_http_request,
    res: *mut sys::cwist_http_response,
    next: sys::cwist_handler_func,
    user_ctx: *mut c_void,
) {
    let (Some(req), Some(res)) = (NonNull::new(req), NonNull::new(res)) else {
        return;
    };
    let outcome = panic::catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: user_ctx is the MiddlewareCtx registered with this middleware,
        // alive until its destructor runs, which CWIST never does during a call.
        let ctx = unsafe { &*(user_ctx as *const MiddlewareCtx) };
        // SAFETY: CWIST owns req/res for the duration of this call and does
        // not touch them while the middleware runs.
        let request = unsafe { Request::from_raw(req) };
        let mut response = unsafe { Response::from_raw(res) };
        // CWIST's next advances a per-request cursor, so a second call would
        // skip ahead or run the route handler again; only the first one counts.
        let called = Cell::new(false);
        let next_fn = || {
            if called.replace(true) {
                return;
            }
            if let Some(f) = next {
                // SAFETY: next is a valid CWIST chain function; req/res remain
                // live for the call, and Response holds no reference into res
                // that the rest of the chain could invalidate.
                unsafe { f(req.as_ptr(), res.as_ptr()) };
            }
        };
        (ctx.middleware)(&request, &mut response, &next_fn);
    }));
    if let Err(payload) = outcome {
        // As in route_trampoline, the payload's Drop can panic again.
        std::mem::forget(payload);
        // Do not continue the chain: a panicking middleware has no way to
        // safely decide whether to call next. Answer with 500 directly.
        let mut response = unsafe { Response::from_raw(res) };
        response.set_status(500);
        let _ = response.set_body("Internal Server Error");
    }
}

/// C destructor for a middleware context: drops the boxed middleware. A panic
/// in the middleware's own Drop is contained.
unsafe extern "C" fn drop_middleware_ctx(user_ctx: *mut c_void) {
    if user_ctx.is_null() {
        return;
    }
    let result = panic::catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: user_ctx came from Box::into_raw in App::use_middleware, and
        // CWIST calls this destructor exactly once per context.
        drop(unsafe { Box::from_raw(user_ctx as *mut MiddlewareCtx) });
    }));
    // The payload's own Drop could panic again; leak it instead.
    if let Err(payload) = result {
        std::mem::forget(payload);
    }
}
