//! Built-in CWIST middleware factories exposed as ordinary Rust values.
//!
//! Each function returns a C middleware function pointer that can be passed to
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

use cwist_sys as sys;
use std::ffi::CString;
use std::sync::Once;

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
pub fn request_id(_header_name: Option<&str>) -> sys::cwist_middleware_func {
    // SAFETY: cwist_mw_request_id returns a static function pointer; the
    // header_name argument is currently unused.
    unsafe { sys::cwist_mw_request_id(std::ptr::null()) }
}

/// Access-log middleware.
pub fn access_log(format: LogFormat) -> sys::cwist_middleware_func {
    // SAFETY: cwist_mw_access_log returns a static function pointer.
    unsafe { sys::cwist_mw_access_log(format.to_sys()) }
}

/// Per-IP fixed-window rate limiter.
///
/// `requests_per_minute` defaults to 60 if zero or negative.
pub fn rate_limit_ip(requests_per_minute: i32) -> sys::cwist_middleware_func {
    // SAFETY: cwist_mw_rate_limit_ip returns a static function pointer.
    unsafe { sys::cwist_mw_rate_limit_ip(requests_per_minute) }
}

/// Prometheus metrics collection middleware.
pub fn metrics() -> sys::cwist_middleware_func {
    // SAFETY: cwist_mw_metrics returns a static function pointer.
    unsafe { sys::cwist_mw_metrics() }
}

/// CORS middleware: adds permissive headers and short-circuits OPTIONS
/// preflight with 204 No Content.
pub fn cors() -> sys::cwist_middleware_func {
    // SAFETY: cwist_mw_cors returns a static function pointer.
    unsafe { sys::cwist_mw_cors() }
}

/// JWT bearer-token authentication middleware.
///
/// CWIST keeps at most eight distinct secrets per process. Past that this
/// returns `None`, which [`App::use_builtin_middleware`](crate::App::use_builtin_middleware)
/// rejects with an error instead of serving without authentication.
///
/// # Safety / Lifetime
///
/// The C factory borrows `secret` for the lifetime of the application. The
/// safe API therefore requires `&'static str`. If you need a dynamic secret,
/// leak a [`CString`] with `into_raw` and accept that it will live until the
/// process exits.
pub fn jwt_auth(secret: &'static str) -> sys::cwist_middleware_func {
    let secret = CString::new(secret).expect("JWT secret contains interior NUL");
    // SAFETY: cwist_mw_jwt_auth returns a static function pointer and borrows
    // the secret for the application lifetime. We leak the CString to satisfy
    // that contract.
    let ptr = secret.into_raw();
    unsafe { sys::cwist_mw_jwt_auth(ptr) }
}

/// Response compression middleware.
///
/// Compresses successful (2xx) responses with the first encoding the client
/// accepts, in the order gzip, deflate, br, zstd, and sets `Content-Encoding`.
/// Responses with a body smaller than `min_body_size` bytes, and deferred
/// responses (see [`AsyncResponse`](crate::AsyncResponse)), are sent as they
/// are. CWIST keeps one threshold per process: the last call sets it for
/// every app.
pub fn compress(min_body_size: usize) -> sys::cwist_middleware_func {
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
    unsafe { sys::cwist_mw_compress(min_body_size) }
}
