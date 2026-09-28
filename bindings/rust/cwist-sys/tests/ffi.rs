//! End-to-end checks through the raw bindings against the real libcwist:
//! a Rust handler registered with a user context, driven by the in-memory
//! dispatcher (no sockets), reading request fields and writing the response
//! through the bindgen-generated struct layouts.

use cwist_sys::*;
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_void};
use std::ptr;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;

fn cstr(s: &str) -> CString {
    CString::new(s).expect("no interior NUL")
}

/// Consumes an error value the way C callers must: check, then dispose.
fn ok(mut err: cwist_error_t) -> bool {
    let ok = unsafe { cwist_error_is_ok_extern(&err) };
    unsafe { cwist_error_dispose(&mut err) };
    ok
}

/// Per-route state handed to C as `user_ctx`.
struct Greeter {
    greeting: String,
    calls: AtomicUsize,
    dropped: Arc<AtomicUsize>,
}

impl Drop for Greeter {
    fn drop(&mut self) {
        self.dropped.fetch_add(1, Ordering::SeqCst);
    }
}

fn boxed_greeter(greeting: &str, dropped: &Arc<AtomicUsize>) -> *mut c_void {
    Box::into_raw(Box::new(Greeter {
        greeting: greeting.to_owned(),
        calls: AtomicUsize::new(0),
        dropped: Arc::clone(dropped),
    })) as *mut c_void
}

unsafe extern "C" fn drop_greeter(user_ctx: *mut c_void) {
    drop(Box::from_raw(user_ctx as *mut Greeter));
}

/// Replies "<greeting>[ <id>][ via <X-Client>]" with status 200.
unsafe extern "C" fn greet(
    user_ctx: *mut c_void,
    req: *mut cwist_http_request,
    res: *mut cwist_http_response,
) {
    let greeter = &*(user_ctx as *const Greeter);
    greeter.calls.fetch_add(1, Ordering::SeqCst);

    let mut body = greeter.greeting.clone();
    let key = cstr("id");
    if !(*req).path_params.is_null() {
        let id = cwist_query_map_get((*req).path_params, key.as_ptr());
        if !id.is_null() {
            body.push(' ');
            body.push_str(&CStr::from_ptr(id).to_string_lossy());
        }
    }
    let header = cstr("X-Client");
    let client = cwist_http_header_get((*req).headers, header.as_ptr());
    if !client.is_null() {
        body.push_str(" via ");
        body.push_str(&CStr::from_ptr(client).to_string_lossy());
    }

    (*res).status_code = CWIST_HTTP_OK;
    let assigned = cwist_sstring_assign_len((*res).body, body.as_ptr() as *const c_char, body.len());
    assert!(ok(assigned));
}

/// Runs one raw HTTP/1.1 request through the app with no socket involved.
fn dispatch(app: *mut cwist_app, request: &str) -> String {
    let mut out: *mut c_char = ptr::null_mut();
    let mut out_len: usize = 0;
    let rc = unsafe {
        cwist_app_dispatch_memory(
            app,
            request.as_ptr() as *const c_char,
            request.len(),
            &mut out,
            &mut out_len,
        )
    };
    assert_eq!(rc, 0, "dispatch failed for {request:?}");
    assert!(!out.is_null());
    let bytes = unsafe { std::slice::from_raw_parts(out as *const u8, out_len) }.to_vec();
    unsafe { cwist_free(out as *mut c_void) };
    String::from_utf8(bytes).expect("response is UTF-8")
}

fn body_of(response: &str) -> &str {
    response.split_once("\r\n\r\n").map(|(_, body)| body).unwrap_or("")
}

#[test]
fn error_values_and_inline_exports() {
    let mut err = unsafe { make_error(CWIST_ERR_INT16) };
    assert_eq!(err.errtype, CWIST_ERR_INT16);
    assert!(unsafe { cwist_error_is_ok_extern(&err) });
    err.error.err_i16 = -1;
    assert!(!unsafe { cwist_error_is_ok_extern(&err) });
    // NULL is success, as for the inline helper.
    assert!(unsafe { cwist_error_is_ok_extern(ptr::null()) });

    assert!(unsafe { cwist_endpoint_has_extern(CWIST_ENDPOINT_DEFAULT, CWIST_DYNAMIC) });
    assert!(!unsafe { cwist_endpoint_has_extern(CWIST_DYNAMIC, CWIST_ENDPOINT_FILE) });
}

#[test]
fn context_route_serves_requests_and_releases_context_once() {
    let dropped = Arc::new(AtomicUsize::new(0));
    let hello = boxed_greeter("hello", &dropped);
    let user = boxed_greeter("user", &dropped);
    let (hello_path, user_path) = (cstr("/hello"), cstr("/users/:id"));

    let app = unsafe { cwist_app_create() };
    assert!(!app.is_null());
    unsafe {
        assert!(ok(cwist_app_get_ex(app, hello_path.as_ptr(), Some(greet), hello, Some(drop_greeter))));
        assert!(ok(cwist_app_get_ex(app, user_path.as_ptr(), Some(greet), user, Some(drop_greeter))));
    }

    let res = dispatch(app, "GET /hello HTTP/1.1\r\nHost: localhost\r\nX-Client: rust\r\n\r\n");
    assert!(res.starts_with("HTTP/1.1 200"), "{res}");
    assert_eq!(body_of(&res), "hello via rust");

    let res = dispatch(app, "GET /users/42 HTTP/1.1\r\nHost: localhost\r\n\r\n");
    assert!(res.starts_with("HTTP/1.1 200"), "{res}");
    assert_eq!(body_of(&res), "user 42");

    let res = dispatch(app, "GET /missing HTTP/1.1\r\nHost: localhost\r\n\r\n");
    assert!(res.starts_with("HTTP/1.1 404"), "{res}");

    unsafe {
        assert_eq!((*(hello as *const Greeter)).calls.load(Ordering::SeqCst), 1);
        assert_eq!((*(user as *const Greeter)).calls.load(Ordering::SeqCst), 1);
    }
    assert_eq!(dropped.load(Ordering::SeqCst), 0, "released while the app is alive");

    unsafe { cwist_app_destroy(app) };
    assert_eq!(dropped.load(Ordering::SeqCst), 2, "each context released exactly once");
}

#[test]
fn replacing_a_route_releases_the_old_context() {
    let dropped = Arc::new(AtomicUsize::new(0));
    let path = cstr("/greet");
    let app = unsafe { cwist_app_create() };
    unsafe {
        let first = boxed_greeter("first", &dropped);
        assert!(ok(cwist_app_get_ex(app, path.as_ptr(), Some(greet), first, Some(drop_greeter))));
        let second = boxed_greeter("second", &dropped);
        assert!(ok(cwist_app_get_ex(app, path.as_ptr(), Some(greet), second, Some(drop_greeter))));
    }
    assert_eq!(dropped.load(Ordering::SeqCst), 1);
    let res = dispatch(app, "GET /greet HTTP/1.1\r\nHost: localhost\r\n\r\n");
    assert_eq!(body_of(&res), "second");
    unsafe { cwist_app_destroy(app) };
    assert_eq!(dropped.load(Ordering::SeqCst), 2);
}

#[test]
fn failed_registration_still_releases_the_context() {
    let dropped = Arc::new(AtomicUsize::new(0));
    let path = cstr("/x");
    let ctx = boxed_greeter("never served", &dropped);
    let err = unsafe {
        cwist_app_get_ex(ptr::null_mut(), path.as_ptr(), Some(greet), ctx, Some(drop_greeter))
    };
    assert!(!ok(err));
    assert_eq!(dropped.load(Ordering::SeqCst), 1);
}

#[test]
fn null_destructor_leaves_ownership_with_the_caller() {
    let dropped = Arc::new(AtomicUsize::new(0));
    let path = cstr("/mine");
    let ctx = boxed_greeter("mine", &dropped);
    let app = unsafe { cwist_app_create() };
    assert!(unsafe { ok(cwist_app_get_ex(app, path.as_ptr(), Some(greet), ctx, None)) });
    let res = dispatch(app, "GET /mine HTTP/1.1\r\nHost: localhost\r\n\r\n");
    assert_eq!(body_of(&res), "mine");
    unsafe { cwist_app_destroy(app) };
    assert_eq!(dropped.load(Ordering::SeqCst), 0);
    // Still ours to release.
    unsafe { drop_greeter(ctx) };
    assert_eq!(dropped.load(Ordering::SeqCst), 1);
}
