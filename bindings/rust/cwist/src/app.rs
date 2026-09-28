use crate::http::{consume, Request, Response};
use crate::Error;
use cwist_sys as sys;
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
    if outcome.is_err() {
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
