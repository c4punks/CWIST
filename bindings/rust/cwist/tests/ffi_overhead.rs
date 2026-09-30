//! Micro-benchmark: safe Rust wrapper dispatch vs raw cwist-sys dispatch.
//!
//! Run with `cargo test --test ffi_overhead -- --nocapture` to see the printed
//! per-call times. The test always passes; it exists to keep the measurement
//! in CI and catch unexpected regressions.

use cwist::{App, Request, Response};
use cwist_sys::*;
use std::ffi::CString;
use std::os::raw::{c_char, c_void};
use std::ptr;
use std::time::{Duration, Instant};

const N: usize = 100_000;
const REQUEST: &str = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";

/// Raw C handler that answers "hello".
unsafe extern "C" fn raw_greet(
    _user_ctx: *mut c_void,
    req: *mut cwist_http_request,
    res: *mut cwist_http_response,
) {
    let _ = req;
    (*res).status_code = CWIST_HTTP_OK;
    let body = cstr("hello");
    let assigned = cwist_sstring_assign_len((*res).body, body.as_ptr(), body.as_bytes().len());
    assert!(ok(assigned));
}

fn cstr(s: &str) -> CString {
    CString::new(s).expect("no interior NUL")
}

fn ok(mut err: cwist_error_t) -> bool {
    let ok = unsafe { cwist_error_is_ok_extern(&err) };
    unsafe { cwist_error_dispose(&mut err) };
    ok
}

fn raw_dispatch_bench() -> Duration {
    let app = unsafe { cwist_app_create() };
    let path = cstr("/hello");
    unsafe {
        assert!(ok(cwist_app_get_ex(
            app,
            path.as_ptr(),
            Some(raw_greet),
            ptr::null_mut(),
            None,
        )));
    }

    let req = cstr(REQUEST);
    let mut out: *mut c_char = ptr::null_mut();
    let mut out_len: usize = 0;

    // Warmup
    for _ in 0..1_000 {
        unsafe {
            cwist_app_dispatch_memory(app, req.as_ptr(), req.as_bytes().len(), &mut out, &mut out_len);
            cwist_free(out as *mut c_void);
        }
    }

    let start = Instant::now();
    for _ in 0..N {
        unsafe {
            cwist_app_dispatch_memory(app, req.as_ptr(), req.as_bytes().len(), &mut out, &mut out_len);
            cwist_free(out as *mut c_void);
        }
    }
    let elapsed = start.elapsed();
    unsafe { cwist_app_destroy(app) };
    elapsed
}

fn safe_dispatch_bench() -> Duration {
    let mut app = App::new().expect("app");
    app.get("/hello", |_req: &Request, res: &mut Response| {
        res.set_status(200);
        res.set_body("hello").unwrap();
    })
    .expect("register");

    let request = REQUEST.as_bytes();

    // Warmup
    for _ in 0..1_000 {
        let _ = app.dispatch(request);
    }

    let start = Instant::now();
    for _ in 0..N {
        let _ = app.dispatch(request);
    }
    start.elapsed()
}

#[test]
fn safe_wrapper_overhead_vs_raw_ffi() {
    let raw = raw_dispatch_bench();
    let safe = safe_dispatch_bench();

    let raw_ns = raw.as_nanos() as f64 / N as f64;
    let safe_ns = safe.as_nanos() as f64 / N as f64;
    let overhead_ns = safe_ns - raw_ns;

    println!("FFI dispatch overhead ({} calls):", N);
    println!("  raw  CWIST C      : {raw_ns:.1} ns/call");
    println!("  safe cwist Rust   : {safe_ns:.1} ns/call");
    println!("  overhead          : {overhead_ns:.1} ns/call");

    // The safe wrapper adds two C crossings (trampoline + panic boundary)
    // and a Box allocation per route. A 500 ns ceiling is generous on CI
    // runners and catches gross regressions.
    assert!(overhead_ns < 500.0, "wrapper overhead regressed: {overhead_ns:.1} ns/call");
}
