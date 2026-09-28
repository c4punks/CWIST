//! The safe API against the real libcwist: routing, request/response access,
//! handler ownership and panic containment, driven through in-memory dispatch.

use cwist::{App, Error, Method};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;

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
fn dispatch_reports_a_malformed_request() {
    let app = App::new().unwrap();
    assert_eq!(app.dispatch(b"this is not http"), Err(Error::Dispatch));
}
