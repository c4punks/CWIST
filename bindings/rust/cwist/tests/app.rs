//! The safe API against the real libcwist: routing, request/response access,
//! handler ownership and panic containment, driven through in-memory dispatch.

use cwist::{App, Error, Method};
use std::ffi::{c_void, CStr, CString};
use std::ptr;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};

fn request(method: &str, target: &str, extra: &str, body: &str) -> Vec<u8> {
    format!(
        "{method} {target} HTTP/1.1\r\nHost: localhost\r\n{extra}Content-Length: {}\r\n\r\n{body}",
        body.len()
    )
    .into_bytes()
}

fn get(app: &App, target: &str) -> String {
    String::from_utf8(app.dispatch(&request("GET", target, "", "")).expect("dispatch"))
        .expect("UTF-8 response")
}

fn status_line(response: &str) -> &str {
    response.lines().next().unwrap_or("")
}

fn body(response: &str) -> &str {
    response.split_once("\r\n\r\n").map(|(_, b)| b).unwrap_or("")
}

/// Counts how many times the value it is moved into is dropped.
struct DropCounter(Arc<AtomicUsize>);

impl Drop for DropCounter {
    fn drop(&mut self) {
        self.0.fetch_add(1, Ordering::SeqCst);
    }
}

#[test]
fn routes_read_the_request_and_write_the_response() {
    let mut app = App::new().expect("app");
    app.get("/users/:id", |req, res| {
        assert_eq!(req.method(), Method::Get);
        let id = req.param("id").unwrap_or("?");
        let fmt = req.query("fmt").unwrap_or("plain");
        let client = req.header("x-client").unwrap_or("none");
        res.set_status(200);
        res.add_header("X-Route", "users").expect("header");
        res.set_body(format!("user {id} as {fmt} from {client}")).expect("body");
    })
    .expect("register GET");
    app.post("/echo", |req, res| {
        assert_eq!(req.method(), Method::Post);
        assert_eq!(req.path(), Some("/echo"));
        res.set_status(201);
        res.set_body(req.body()).expect("body");
    })
    .expect("register POST");

    let raw = app
        .dispatch(&request("GET", "/users/42?fmt=json", "X-Client: rust\r\n", ""))
        .expect("dispatch");
    let res = String::from_utf8(raw).unwrap();
    assert!(status_line(&res).starts_with("HTTP/1.1 200"), "{res}");
    assert!(res.contains("X-Route: users\r\n"), "{res}");
    assert_eq!(body(&res), "user 42 as json from rust");

    let res = String::from_utf8(app.dispatch(&request("POST", "/echo", "", "ping")).unwrap()).unwrap();
    assert!(status_line(&res).starts_with("HTTP/1.1 201"), "{res}");
    assert_eq!(body(&res), "ping");

    assert!(status_line(&get(&app, "/nowhere")).starts_with("HTTP/1.1 404"));
}

#[test]
fn every_method_routes_to_its_own_handler() {
    let mut app = App::new().unwrap();
    app.put("/r", |_, res| res.set_body("put").unwrap()).unwrap();
    app.delete("/r", |_, res| res.set_body("delete").unwrap()).unwrap();
    app.patch("/r", |_, res| res.set_body("patch").unwrap()).unwrap();
    for method in ["PUT", "DELETE", "PATCH"] {
        let res = String::from_utf8(app.dispatch(&request(method, "/r", "", "")).unwrap()).unwrap();
        assert_eq!(body(&res), method.to_lowercase(), "{method}: {res}");
    }
}

#[test]
fn handlers_are_dropped_exactly_once_when_the_app_is_dropped() {
    let drops = Arc::new(AtomicUsize::new(0));
    let calls = Arc::new(AtomicUsize::new(0));
    {
        let mut app = App::new().unwrap();
        for path in ["/a", "/b/:id"] {
            let guard = DropCounter(Arc::clone(&drops));
            let calls = Arc::clone(&calls);
            app.get(path, move |_, res| {
                let _keep = &guard;
                calls.fetch_add(1, Ordering::SeqCst);
                res.set_body("ok").unwrap();
            })
            .unwrap();
        }
        get(&app, "/a");
        get(&app, "/b/1");
        assert_eq!(drops.load(Ordering::SeqCst), 0, "dropped while the app is alive");
    }
    assert_eq!(calls.load(Ordering::SeqCst), 2);
    assert_eq!(drops.load(Ordering::SeqCst), 2);
}

#[test]
fn replacing_a_route_drops_the_old_handler_once() {
    let drops = Arc::new(AtomicUsize::new(0));
    let mut app = App::new().unwrap();
    let first = DropCounter(Arc::clone(&drops));
    app.get("/v", move |_, res| {
        let _keep = &first;
        res.set_body("first").unwrap();
    })
    .unwrap();
    let second = DropCounter(Arc::clone(&drops));
    app.get("/v", move |_, res| {
        let _keep = &second;
        res.set_body("second").unwrap();
    })
    .unwrap();
    assert_eq!(drops.load(Ordering::SeqCst), 1);
    assert_eq!(body(&get(&app, "/v")), "second");
    drop(app);
    assert_eq!(drops.load(Ordering::SeqCst), 2);
}

#[test]
fn a_rejected_registration_drops_the_handler_once() {
    let drops = Arc::new(AtomicUsize::new(0));
    let mut app = App::new().unwrap();
    let guard = DropCounter(Arc::clone(&drops));
    let err = app
        .get("/bad\0path", move |_, _| {
            let _keep = &guard;
        })
        .unwrap_err();
    assert_eq!(err, Error::InteriorNul("route path"));
    assert_eq!(drops.load(Ordering::SeqCst), 1);
    drop(app);
    assert_eq!(drops.load(Ordering::SeqCst), 1, "never handed to CWIST");
}

#[test]
fn cwist_rejects_header_injection_through_add_header() {
    let mut app = App::new().unwrap();
    app.get("/h", |_, res| {
        assert_eq!(res.add_header("X-Bad", "a\r\nSet-Cookie: x=1"), Err(Error::Header));
        assert_eq!(res.add_header("X-Nul", "a\0b"), Err(Error::InteriorNul("header value")));
        res.add_header("X-Good", "fine").unwrap();
        res.set_body("done").unwrap();
    })
    .unwrap();
    let res = get(&app, "/h");
    assert!(res.contains("X-Good: fine\r\n"), "{res}");
    assert!(!res.contains("Set-Cookie: x=1"), "{res}");
}

#[test]
fn a_panicking_handler_becomes_a_500_and_the_app_keeps_serving() {
    let mut app = App::new().unwrap();
    app.get("/boom", |_, res| {
        res.set_body("partial").unwrap();
        panic!("handler failure");
    })
    .unwrap();
    app.get("/fine", |_, res| res.set_body("still here").unwrap()).unwrap();

    let res = get(&app, "/boom");
    assert!(status_line(&res).starts_with("HTTP/1.1 500"), "{res}");
    assert_eq!(body(&res), "Internal Server Error");

    let res = get(&app, "/fine");
    assert!(status_line(&res).starts_with("HTTP/1.1 200"), "{res}");
    assert_eq!(body(&res), "still here");
    // The panic did not poison anything: the same route can fail again.
    assert!(status_line(&get(&app, "/boom")).starts_with("HTTP/1.1 500"));
}

/// Panics while being dropped, which happens inside the C destructor.
struct PanicOnDrop;

impl Drop for PanicOnDrop {
    fn drop(&mut self) {
        panic!("drop failure");
    }
}

#[test]
fn a_panic_while_dropping_a_handler_does_not_cross_into_c() {
    // An unwind escaping the destructor callback would abort the whole test
    // process, not fail this test.
    let mut app = App::new().unwrap();
    let guard = PanicOnDrop;
    app.get("/p", move |_, res| {
        let _keep = &guard;
        res.set_body("ok").unwrap();
    })
    .unwrap();
    assert_eq!(body(&get(&app, "/p")), "ok");
    drop(app);
}

#[test]
fn a_panicking_payload_does_not_abort_request_dispatch() {
    const CHILD: &str = "CWIST_TEST_PANICKING_PAYLOAD_CHILD";
    if std::env::var_os(CHILD).is_none() {
        // An unwind from an extern "C" callback aborts the process. Isolate
        // the request so a regression fails this test, not the entire suite.
        let output = std::process::Command::new(std::env::current_exe().unwrap())
            .args(["--exact", "a_panicking_payload_does_not_abort_request_dispatch", "--nocapture"])
            .env(CHILD, "1")
            .output()
            .expect("run panic payload child");
        assert!(
            output.status.success(),
            "child status: {}\nstdout:\n{}\nstderr:\n{}",
            output.status,
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        );
        return;
    }

    let mut app = App::new().unwrap();
    app.get("/boom", |_, res| {
        res.set_body("partial").unwrap();
        std::panic::panic_any(PanicOnDrop);
    })
    .unwrap();
    app.get("/fine", |_, res| res.set_body("still here").unwrap()).unwrap();

    for _ in 0..2 {
        let res = get(&app, "/boom");
        assert!(status_line(&res).starts_with("HTTP/1.1 500"), "{res}");
        assert_eq!(body(&res), "Internal Server Error");
        let res = get(&app, "/fine");
        assert!(status_line(&res).starts_with("HTTP/1.1 200"), "{res}");
        assert_eq!(body(&res), "still here");
    }
}

#[test]
fn dispatch_reports_a_malformed_request() {
    let app = App::new().unwrap();
    assert_eq!(app.dispatch(b"this is not http"), Err(Error::Dispatch));
}

#[test]
fn middleware_reads_the_request_and_edits_the_response_around_next() {
    let mut app = App::new().unwrap();
    app.use_middleware(|req, res, next| {
        assert_eq!(req.method(), Method::Get);
        let client = req.header("x-client").unwrap_or("none").to_owned();
        res.add_header("X-Before", &client).expect("header");
        next();
        // The response is serialized after the whole chain returns, so
        // changes made after next are sent too.
        assert_eq!(res.status(), 200);
        res.set_status(202);
        res.add_header("X-After", "yes").expect("header");
    })
    .unwrap();
    app.get("/r", |_req, res| {
        res.set_status(200);
        res.add_header("X-Route", "ok").unwrap();
        res.set_body("body").unwrap();
    })
    .unwrap();

    let raw = app.dispatch(&request("GET", "/r", "X-Client: rust\r\n", "")).unwrap();
    let res = String::from_utf8(raw).unwrap();
    assert!(status_line(&res).starts_with("HTTP/1.1 202"), "{res}");
    assert!(res.contains("X-Before: rust\r\n"), "{res}");
    assert!(res.contains("X-Route: ok\r\n"), "{res}");
    assert!(res.contains("X-After: yes\r\n"), "{res}");
    assert_eq!(body(&res), "body");
}

#[test]
fn middleware_can_short_circuit_the_chain() {
    let calls = Arc::new(AtomicUsize::new(0));
    let mut app = App::new().unwrap();
    app.use_middleware(|req, res, next| {
        if req.path() == Some("/blocked") {
            res.set_status(403);
            res.set_body("forbidden").unwrap();
            return;
        }
        next();
    })
    .unwrap();
    for path in ["/blocked", "/open"] {
        let calls = Arc::clone(&calls);
        app.get(path, move |_req, res| {
            calls.fetch_add(1, Ordering::SeqCst);
            res.set_body("handler").unwrap();
        })
        .unwrap();
    }

    let res = get(&app, "/blocked");
    assert!(status_line(&res).starts_with("HTTP/1.1 403"), "{res}");
    assert_eq!(body(&res), "forbidden");
    assert_eq!(calls.load(Ordering::SeqCst), 0, "the handler ran behind a short-circuit");

    let res = get(&app, "/open");
    assert_eq!(body(&res), "handler");
    assert_eq!(calls.load(Ordering::SeqCst), 1);
}

#[test]
fn middleware_runs_in_registration_order() {
    let trace = Arc::new(Mutex::new(Vec::new()));
    let mut app = App::new().unwrap();
    for (before, after) in [("a", "A"), ("b", "B")] {
        let trace = Arc::clone(&trace);
        app.use_middleware(move |_req, _res, next| {
            trace.lock().unwrap().push(before);
            next();
            trace.lock().unwrap().push(after);
        })
        .unwrap();
    }
    let handler_trace = Arc::clone(&trace);
    app.get("/r", move |_req, res| {
        handler_trace.lock().unwrap().push("handler");
        res.set_body("done").unwrap();
    })
    .unwrap();
    // Middleware registered after a route still runs for it.
    let late_trace = Arc::clone(&trace);
    app.use_middleware(move |_req, _res, next| {
        late_trace.lock().unwrap().push("c");
        next();
    })
    .unwrap();

    assert_eq!(body(&get(&app, "/r")), "done");
    assert_eq!(*trace.lock().unwrap(), ["a", "b", "c", "handler", "B", "A"]);
}

#[test]
fn next_runs_the_rest_of_the_chain_only_once() {
    let calls = Arc::new(AtomicUsize::new(0));
    let mut app = App::new().unwrap();
    app.use_middleware(|_req, _res, next| {
        next();
        next();
    })
    .unwrap();
    let handler_calls = Arc::clone(&calls);
    app.get("/r", move |_req, res| {
        handler_calls.fetch_add(1, Ordering::SeqCst);
        res.set_body("once").unwrap();
    })
    .unwrap();

    assert_eq!(body(&get(&app, "/r")), "once");
    assert_eq!(calls.load(Ordering::SeqCst), 1);
}

#[test]
fn middleware_is_dropped_exactly_once_when_the_app_is_dropped() {
    let drops = Arc::new(AtomicUsize::new(0));
    {
        let mut app = App::new().unwrap();
        for _ in 0..2 {
            let guard = DropCounter(Arc::clone(&drops));
            app.use_middleware(move |_req, _res, next| {
                let _keep = &guard;
                next();
            })
            .unwrap();
        }
        get(&app, "/");
        get(&app, "/");
        assert_eq!(drops.load(Ordering::SeqCst), 0, "dropped while the app is alive");
    }
    assert_eq!(drops.load(Ordering::SeqCst), 2);
}

/// Counts calls of the destructor below; only the test that uses it touches it.
static REJECTED_DROPS: AtomicUsize = AtomicUsize::new(0);

unsafe extern "C" fn count_rejected_drop(ctx: *mut c_void) {
    // SAFETY: ctx is the Box<DropCounter> the test handed to CWIST.
    drop(unsafe { Box::from_raw(ctx.cast::<DropCounter>()) });
    REJECTED_DROPS.fetch_add(1, Ordering::SeqCst);
}

unsafe extern "C" fn never_called(
    _req: *mut cwist_sys::cwist_http_request,
    _res: *mut cwist_sys::cwist_http_response,
    _next: cwist_sys::cwist_handler_func,
    _ctx: *mut c_void,
) {
    std::process::abort();
}

#[test]
fn a_rejected_middleware_context_is_released_exactly_once() {
    // App::use_middleware relies on this C contract: a rejected registration
    // runs the destructor once before returning and keeps nothing.
    let drops = Arc::new(AtomicUsize::new(0));
    let ctx = Box::into_raw(Box::new(DropCounter(Arc::clone(&drops)))).cast::<c_void>();
    // SAFETY: a NULL app is rejected; ownership of ctx passes to CWIST, which
    // releases it through count_rejected_drop.
    let mut err = unsafe {
        cwist_sys::cwist_app_use_ex(
            ptr::null_mut(),
            None,
            Some(never_called),
            ctx,
            Some(count_rejected_drop),
        )
    };
    // SAFETY: err is the live error value returned above, disposed once.
    let ok = unsafe { cwist_sys::cwist_error_is_ok_extern(&err) };
    unsafe { cwist_sys::cwist_error_dispose(&mut err) };
    assert!(!ok);
    assert_eq!(REJECTED_DROPS.load(Ordering::SeqCst), 1);
    assert_eq!(drops.load(Ordering::SeqCst), 1);
}

#[test]
fn a_panicking_middleware_becomes_a_500_and_the_route_is_not_reached() {
    let drops = Arc::new(AtomicUsize::new(0));
    let calls = Arc::new(AtomicUsize::new(0));
    {
        let mut app = App::new().unwrap();
        let guard = DropCounter(Arc::clone(&drops));
        app.use_middleware(move |_req, _res, _next| {
            let _keep = &guard;
            panic!("middleware failure");
        })
        .unwrap();
        let handler_calls = Arc::clone(&calls);
        app.get("/fine", move |_req, res| {
            handler_calls.fetch_add(1, Ordering::SeqCst);
            res.set_body("still here").unwrap();
        })
        .unwrap();

        let res = get(&app, "/fine");
        assert!(status_line(&res).starts_with("HTTP/1.1 500"), "{res}");
        assert_eq!(body(&res), "Internal Server Error");
        // The app keeps serving after a contained panic.
        let res = get(&app, "/fine");
        assert!(status_line(&res).starts_with("HTTP/1.1 500"), "{res}");
        assert_eq!(calls.load(Ordering::SeqCst), 0);
        assert_eq!(drops.load(Ordering::SeqCst), 0);
    }
    assert_eq!(drops.load(Ordering::SeqCst), 1);
}

#[test]
fn a_panic_after_next_replaces_the_response_with_a_500() {
    let calls = Arc::new(AtomicUsize::new(0));
    let mut app = App::new().unwrap();
    app.use_middleware(|_req, _res, next| {
        next();
        panic!("after next");
    })
    .unwrap();
    let handler_calls = Arc::clone(&calls);
    app.get("/r", move |_req, res| {
        handler_calls.fetch_add(1, Ordering::SeqCst);
        res.set_body("handler").unwrap();
    })
    .unwrap();

    let res = get(&app, "/r");
    assert!(status_line(&res).starts_with("HTTP/1.1 500"), "{res}");
    assert_eq!(body(&res), "Internal Server Error");
    assert_eq!(calls.load(Ordering::SeqCst), 1);
}

#[test]
fn dispatch_refuses_to_defer_and_the_handler_answers_itself() {
    let mut app = App::new().unwrap();
    app.get("/sync", |req, res| {
        // In-memory dispatch frees the request and response when the handler
        // returns, so nothing can take them over.
        assert!(cwist::AsyncResponse::defer(req, res).is_none());
        assert!(!res.is_deferred());
        res.set_body("answered inline").unwrap();
    })
    .unwrap();

    let res = get(&app, "/sync");
    assert!(status_line(&res).starts_with("HTTP/1.1 200"), "{res}");
    assert_eq!(body(&res), "answered inline");
}

/// Signs `payload` with CWIST's own HS256 signer; the token is copied out of
/// the CWIST allocation, which is then released.
fn sign_jwt(payload: &str, secret: &str) -> String {
    let payload = CString::new(payload).unwrap();
    let secret = CString::new(secret).unwrap();
    // SAFETY: both strings are NUL-terminated and live for the call.
    let raw = unsafe { cwist_sys::cwist_jwt_sign(payload.as_ptr(), secret.as_ptr(), 3600) };
    assert!(!raw.is_null(), "cwist_jwt_sign failed");
    // SAFETY: raw is a NUL-terminated string from CWIST's allocator, copied
    // before it is freed and not used afterwards.
    let token = unsafe { CStr::from_ptr(raw) }.to_str().unwrap().to_owned();
    unsafe { cwist_sys::cwist_free(raw.cast()) };
    token
}

/// An app with `builtin`, then a Rust middleware that marks the response
/// after `next`, then a Rust route counting its calls.
fn app_with_builtin(builtin: cwist_sys::cwist_middleware_func, calls: &Arc<AtomicUsize>) -> App {
    let mut app = App::new().unwrap();
    app.use_builtin_middleware(builtin).expect("register built-in middleware");
    app.use_middleware(|_req, res, next| {
        next();
        res.add_header("X-After", "yes").unwrap();
    })
    .unwrap();
    let calls = Arc::clone(calls);
    app.get("/r", move |_req, res| {
        calls.fetch_add(1, Ordering::SeqCst);
        res.set_body("route").unwrap();
    })
    .unwrap();
    app
}

#[test]
fn builtin_rate_limit_runs_the_rest_of_the_chain() {
    let calls = Arc::new(AtomicUsize::new(0));
    let app = app_with_builtin(cwist::middleware::rate_limit_ip(60), &calls);

    // In-memory dispatch has no client address, so the limiter lets the
    // request through to the Rust middleware and route via next().
    for _ in 0..3 {
        let res = get(&app, "/r");
        assert!(status_line(&res).starts_with("HTTP/1.1 200"), "{res}");
        assert!(res.contains("X-After: yes\r\n"), "{res}");
        assert_eq!(body(&res), "route");
    }
    assert_eq!(calls.load(Ordering::SeqCst), 3);
}

#[test]
fn builtin_jwt_auth_guards_rust_routes() {
    const SECRET: &str = "rust-builtin-jwt-secret";
    let calls = Arc::new(AtomicUsize::new(0));
    let app = app_with_builtin(cwist::middleware::jwt_auth(SECRET), &calls);

    let res = get(&app, "/r");
    assert!(status_line(&res).starts_with("HTTP/1.1 401"), "{res}");
    assert!(!res.contains("X-After"), "{res}");

    let bearer = format!("Authorization: Bearer {}\r\n", sign_jwt("{\"sub\":\"alice\"}", "wrong"));
    let res = String::from_utf8(app.dispatch(&request("GET", "/r", &bearer, "")).unwrap()).unwrap();
    assert!(status_line(&res).starts_with("HTTP/1.1 401"), "{res}");
    assert_eq!(calls.load(Ordering::SeqCst), 0);

    // A valid token continues through the chain to the Rust route.
    let bearer = format!("Authorization: Bearer {}\r\n", sign_jwt("{\"sub\":\"alice\"}", SECRET));
    let res = String::from_utf8(app.dispatch(&request("GET", "/r", &bearer, "")).unwrap()).unwrap();
    assert!(status_line(&res).starts_with("HTTP/1.1 200"), "{res}");
    assert!(res.contains("X-After: yes\r\n"), "{res}");
    assert_eq!(body(&res), "route");
    assert_eq!(calls.load(Ordering::SeqCst), 1);
}

#[test]
fn a_missing_builtin_middleware_is_an_error_not_a_silent_skip() {
    let mut app = App::new().unwrap();
    // What a factory returns when it cannot provide the middleware, for
    // example jwt_auth once CWIST's secret slots are used up.
    assert_eq!(app.use_builtin_middleware(None), Err(Error::Middleware));
    app.get("/r", |_req, res| res.set_body("open").unwrap()).unwrap();
    assert_eq!(body(&get(&app, "/r")), "open");
}
