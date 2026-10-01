//! Built-in CWIST middleware factories exposed as ordinary Rust values.
//!
//! Each function returns a [`BuiltinMiddleware`] that can be passed to
//! [`App::use_builtin_middleware`](crate::App::use_builtin_middleware). They do
//! not allocate on the Rust side beyond a few small stack values.
//!
//! ```no_run
//! use cwist::App;
//!
//! let mut app = App::new().unwrap();
//! app.use_builtin_middleware(cwist::middleware::request_id(None)).unwrap();
//! app.use_builtin_middleware(cwist::middleware::access_log(cwist::middleware::LogFormat::Combined))
//!     .unwrap();
//! ```

use crate::Error;
use cwist_sys as sys;
use std::ffi::CString;
use std::sync::Once;

/// A built-in CWIST middleware, ready for
/// [`App::use_builtin_middleware`](crate::App::use_builtin_middleware).
///
/// Only the factories in this module create one, so CWIST only ever calls
/// its own middleware. Any other C function pointer cannot be passed in from
/// safe code; it would need its own unsafe contract:
///
/// ```compile_fail
/// let mut app = cwist::App::new().unwrap();
/// app.use_builtin_middleware(None).unwrap();
/// ```
///
/// ```compile_fail
/// unsafe extern "C" fn mine(
///     _req: *mut cwist_sys::cwist_http_request,
///     _res: *mut cwist_sys::cwist_http_response,
///     _next: cwist_sys::cwist_handler_func,
/// ) {
/// }
/// let mut app = cwist::App::new().unwrap();
/// app.use_builtin_middleware(Some(mine)).unwrap();
/// ```
#[derive(Debug, Clone, Copy)]
pub struct BuiltinMiddleware {
    func: unsafe extern "C" fn(
        *mut sys::cwist_http_request,
        *mut sys::cwist_http_response,
        sys::cwist_handler_func,
    ),
}

impl BuiltinMiddleware {
    /// Wraps what a CWIST factory returned, or `None` if it returned NULL.
    fn from_factory(func: sys::cwist_middleware_func) -> Option<BuiltinMiddleware> {
        func.map(|func| BuiltinMiddleware { func })
    }

    /// For factories that never return NULL.
    fn from_static(func: sys::cwist_middleware_func) -> BuiltinMiddleware {
        Self::from_factory(func).expect("CWIST middleware factory returned NULL")
    }

    pub(crate) fn as_sys(self) -> sys::cwist_middleware_func {
        Some(self.func)
    }
}

/// Access-log output format.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[non_exhaustive]
pub enum LogFormat {
    /// Common log format.
    Common,
    /// Combined log format.
    Combined,
    /// JSON log format.
    Json,
}

impl LogFormat {
    fn to_sys(self) -> sys::cwist_log_format_t {
        match self {
            LogFormat::Common => sys::CWIST_LOG_COMMON,
            LogFormat::Combined => sys::CWIST_LOG_COMBINED,
            LogFormat::Json => sys::CWIST_LOG_JSON,
        }
    }
}

/// Request ID middleware.
///
/// Currently the `header_name` parameter is ignored by CWIST and the header
/// is always `X-Request-Id`.
pub fn request_id(_header_name: Option<&str>) -> BuiltinMiddleware {
    // SAFETY: cwist_mw_request_id returns a static function pointer; the
    // header_name argument is currently unused.
    BuiltinMiddleware::from_static(unsafe { sys::cwist_mw_request_id(std::ptr::null()) })
}

/// Access-log middleware.
pub fn access_log(format: LogFormat) -> BuiltinMiddleware {
    // SAFETY: cwist_mw_access_log returns a static function pointer.
    BuiltinMiddleware::from_static(unsafe { sys::cwist_mw_access_log(format.to_sys()) })
}

/// Per-IP fixed-window rate limiter.
///
/// `requests_per_minute` defaults to 60 if zero or negative.
pub fn rate_limit_ip(requests_per_minute: i32) -> BuiltinMiddleware {
    // SAFETY: cwist_mw_rate_limit_ip returns a static function pointer.
    BuiltinMiddleware::from_static(unsafe { sys::cwist_mw_rate_limit_ip(requests_per_minute) })
}

/// Prometheus metrics collection middleware.
pub fn metrics() -> BuiltinMiddleware {
    // SAFETY: cwist_mw_metrics returns a static function pointer.
    BuiltinMiddleware::from_static(unsafe { sys::cwist_mw_metrics() })
}

/// CORS middleware: adds permissive headers and short-circuits OPTIONS
/// preflight with 204 No Content.
pub fn cors() -> BuiltinMiddleware {
    // SAFETY: cwist_mw_cors returns a static function pointer.
    BuiltinMiddleware::from_static(unsafe { sys::cwist_mw_cors() })
}

/// JWT bearer-token authentication middleware.
///
/// CWIST keeps at most eight distinct secrets per process. Past that this
/// returns [`Error::Middleware`], so an app cannot end up serving without the
/// authentication it asked for. A secret containing a NUL byte returns
/// [`Error::InteriorNul`].
///
/// # Safety / Lifetime
///
/// The C factory borrows `secret` for the lifetime of the application. The
/// safe API therefore requires `&'static str`. If you need a dynamic secret,
/// leak a [`CString`] with `into_raw` and accept that it will live until the
/// process exits.
pub fn jwt_auth(secret: &'static str) -> Result<BuiltinMiddleware, Error> {
    let secret = CString::new(secret).map_err(|_| Error::InteriorNul("JWT secret"))?;
    // SAFETY: cwist_mw_jwt_auth returns a static function pointer (or NULL
    // when its secret slots are full) and borrows the secret for the
    // application lifetime. We leak the CString to satisfy that contract.
    let ptr = secret.into_raw();
    BuiltinMiddleware::from_factory(unsafe { sys::cwist_mw_jwt_auth(ptr) }).ok_or(Error::Middleware)
}

/// Response compression middleware.
///
/// Compresses successful (2xx) responses with the first encoding the client
/// accepts, in the order gzip, deflate, br, zstd, and sets `Content-Encoding`.
/// Responses with a body smaller than `min_body_size` bytes, and deferred
/// responses (see [`AsyncResponse`](crate::AsyncResponse)), are sent as they
/// are. CWIST keeps one threshold per process: the last call sets it for
/// every app.
pub fn compress(min_body_size: usize) -> BuiltinMiddleware {
    // CWIST compresses only with registered backends and registers none by
    // itself. The registry is process-wide and unsynchronised, so register
    // the built-in ones exactly once, here: any server running this
    // middleware got it from an earlier call, which completed the
    // registration first.
    static BACKENDS: Once = Once::new();
    BACKENDS.call_once(|| {
        // SAFETY: the backend getters return static descriptors; the Once
        // makes these the only registry writes from the safe API.
        unsafe {
            sys::cwist_compress_register_backend(sys::cwist_compress_backend_gzip());
            sys::cwist_compress_register_backend(sys::cwist_compress_backend_deflate());
            sys::cwist_compress_register_backend(sys::cwist_compress_backend_brotli());
            sys::cwist_compress_register_backend(sys::cwist_compress_backend_zstd());
        }
    });
    // SAFETY: cwist_mw_compress returns a static function pointer.
    BuiltinMiddleware::from_static(unsafe { sys::cwist_mw_compress(min_body_size) })
}
