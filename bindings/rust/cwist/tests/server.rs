//! `App::listen` and `cwist::shutdown` against a real CWIST server on
//! loopback: serving, graceful shutdown from outside and from a handler,
//! handler ownership across the server lifecycle, and the failure paths.
//!
//! CWIST's server and shutdown state are process-wide, so these tests run
//! one at a time under `SERIAL`.

use cwist::{App, Error};
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{mpsc, Arc, Mutex, MutexGuard};
use std::thread;
use std::time::{Duration, Instant};

static SERIAL: Mutex<()> = Mutex::new(());

/// Takes the serial lock and makes shutdowns skip the drain wait.
fn serial() -> MutexGuard<'static, ()> {
    let guard = SERIAL.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    // SAFETY: only these tests touch the drain timeout; they hold SERIAL and
    // no server is running between tests, so nothing reads it concurrently.
    unsafe { cwist_sys::g_cwist_drain_timeout_sec = 0 };
    guard
}

fn free_port() -> u16 {
    TcpListener::bind("127.0.0.1:0").unwrap().local_addr().unwrap().port()
}

/// Counts how many times the value it is moved into is dropped.
struct DropCounter(Arc<AtomicUsize>);

impl Drop for DropCounter {
    fn drop(&mut self) {
        self.0.fetch_add(1, Ordering::SeqCst);
    }
}

/// Builds an app on a new thread (App is not Send), registers `/ping` plus
/// whatever `setup` adds, and listens; the result arrives on the channel.
fn serve<F>(port: u16, setup: F) -> mpsc::Receiver<Result<(), Error>>
where
    F: FnOnce(&mut App) + Send + 'static,
{
    let (tx, rx) = mpsc::channel();
    thread::spawn(move || {
        let mut app = App::new().expect("app");
        app.get("/ping", |_, res| res.set_body("pong").unwrap()).unwrap();
        setup(&mut app);
        let _ = tx.send(app.listen(port));
    });
    rx
}

/// One GET over a fresh connection: (status, body), or None if the server
/// is not reachable.
fn http_get(port: u16, path: &str) -> Option<(u16, String)> {
    let mut stream = TcpStream::connect(("127.0.0.1", port)).ok()?;
    stream.set_read_timeout(Some(Duration::from_secs(5))).ok()?;
    write!(stream, "GET {path} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n").ok()?;
    let mut response = String::new();
    stream.read_to_string(&mut response).ok()?;
    let status = response.split(' ').nth(1)?.parse().ok()?;
    let body = response.split_once("\r\n\r\n").map(|(_, b)| b.to_owned()).unwrap_or_default();
    Some((status, body))
}

fn wait_until_up(port: u16) -> bool {
    let deadline = Instant::now() + Duration::from_secs(10);
    while Instant::now() < deadline {
        if http_get(port, "/ping").is_some() {
            return true;
        }
        thread::sleep(Duration::from_millis(25));
    }
    false
}

fn finished(rx: &mpsc::Receiver<Result<(), Error>>) -> Result<(), Error> {
    rx.recv_timeout(Duration::from_secs(15)).expect("listen did not return after shutdown")
}

#[test]
fn listen_serves_until_shutdown_is_requested() {
    let _serial = serial();
    let port = free_port();
    let drops = Arc::new(AtomicUsize::new(0));
    let guard = DropCounter(Arc::clone(&drops));
    let done = serve(port, move |app| {
        app.get("/users/:id", move |req, res| {
            let _keep = &guard;
            let id = req.param("id").unwrap_or("?").to_owned();
            res.set_body(format!("user {id}")).unwrap();
        })
        .unwrap();
    });

    assert!(wait_until_up(port), "server did not come up");
    assert_eq!(http_get(port, "/users/7"), Some((200, "user 7".to_owned())));
    assert_eq!(http_get(port, "/missing").map(|(s, _)| s), Some(404));
    assert_eq!(drops.load(Ordering::SeqCst), 0, "handler dropped while serving");

    cwist::shutdown();
    assert_eq!(finished(&done), Ok(()));
    // listen consumed the app: every handler is dropped once it returns.
    assert_eq!(drops.load(Ordering::SeqCst), 1);
    assert!(http_get(port, "/ping").is_none(), "still serving after shutdown");
}

#[test]
fn a_handler_can_stop_its_own_server() {
    let _serial = serial();
    let port = free_port();
    let done = serve(port, |app| {
        app.get("/stop", |_, res| {
            cwist::shutdown();
            cwist::shutdown(); // repeated requests are harmless
            res.set_body("bye").unwrap();
        })
        .unwrap();
    });
    assert!(wait_until_up(port));
    assert_eq!(http_get(port, "/stop"), Some((200, "bye".to_owned())));
    assert_eq!(finished(&done), Ok(()));
}

#[test]
fn a_second_listen_while_serving_is_rejected() {
    let _serial = serial();
    let port = free_port();
    let done = serve(port, |_| {});
    assert!(wait_until_up(port));

    let drops = Arc::new(AtomicUsize::new(0));
    let guard = DropCounter(Arc::clone(&drops));
    let mut second = App::new().unwrap();
    second
        .get("/x", move |_, res| {
            let _keep = &guard;
            res.set_body("x").unwrap();
        })
        .unwrap();
    assert_eq!(second.listen(free_port()), Err(Error::AlreadyListening));
    assert_eq!(drops.load(Ordering::SeqCst), 1, "rejected app is dropped");

    // The running server is unaffected.
    assert_eq!(http_get(port, "/ping"), Some((200, "pong".to_owned())));
    cwist::shutdown();
    assert_eq!(finished(&done), Ok(()));
}

#[test]
fn the_process_can_serve_again_after_a_shutdown() {
    let _serial = serial();
    for round in 0..2 {
        let port = free_port();
        let done = serve(port, |_| {});
        assert!(wait_until_up(port), "round {round}: server did not come up");
        cwist::shutdown();
        assert_eq!(finished(&done), Ok(()), "round {round}");
    }
}

#[test]
fn a_shutdown_requested_before_listen_makes_it_return_at_once() {
    let _serial = serial();
    cwist::shutdown();
    let done = serve(free_port(), |_| {});
    assert_eq!(finished(&done), Ok(()));
}

#[test]
fn listening_on_a_busy_port_is_an_error() {
    let _serial = serial();
    let holder = TcpListener::bind("0.0.0.0:0").unwrap();
    let port = holder.local_addr().unwrap().port();
    let app = App::new().unwrap();
    assert_eq!(app.listen(port), Err(Error::Listen { port }));
    drop(holder);

    // A failed start leaves nothing behind: the next server works.
    let port = free_port();
    let done = serve(port, |_| {});
    assert!(wait_until_up(port));
    cwist::shutdown();
    assert_eq!(finished(&done), Ok(()));
}

#[test]
fn async_handler_completes_from_another_thread() {
    let _serial = serial();
    let port = free_port();
    let (tx, rx) = mpsc::channel::<cwist::AsyncResponse>();
    let done = serve(port, move |app| {
        app.get("/defer", move |_req, res| {
            let defer = cwist::AsyncResponse::defer(_req, res).expect("defer");
            tx.send(defer).expect("send async handle");
        })
        .unwrap();
    });

    assert!(wait_until_up(port));

    let mut stream = TcpStream::connect(("127.0.0.1", port)).unwrap();
    stream
        .set_read_timeout(Some(Duration::from_secs(5)))
        .unwrap();
    write!(
        stream,
        "GET /defer HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
    )
    .unwrap();

    let mut defer = rx.recv_timeout(Duration::from_secs(5)).expect("receive async handle");
    assert!(defer.respond(200, "text/plain", "deferred body"));

    let mut response = String::new();
    stream.read_to_string(&mut response).unwrap();
    assert!(response.starts_with("HTTP/1.1 200"), "{response}");
    assert!(response.contains("deferred body"), "{response}");

    cwist::shutdown();
    assert_eq!(finished(&done), Ok(()));
}

/// Sends one GET and returns the raw response, for checks on headers.
fn raw_get(port: u16, path: &str) -> String {
    let mut stream = TcpStream::connect(("127.0.0.1", port)).unwrap();
    stream.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
    write!(stream, "GET {path} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n").unwrap();
    let mut response = String::new();
    stream.read_to_string(&mut response).unwrap();
    response
}

#[test]
fn dropping_an_unanswered_async_response_answers_500() {
    let _serial = serial();
    let port = free_port();
    let done = serve(port, |app| {
        app.get("/drop", |req, res| {
            let handle = cwist::AsyncResponse::defer(req, res).expect("defer");
            drop(handle);
        })
        .unwrap();
        app.get("/panic", |req, res| {
            let _handle = cwist::AsyncResponse::defer(req, res).expect("defer");
            panic!("handler failure after defer");
        })
        .unwrap();
    });

    assert!(wait_until_up(port));
    assert_eq!(http_get(port, "/drop"), Some((500, "Internal Server Error".to_owned())));
    // A panic unwinds through the handle's Drop: still one 500, not a hang.
    assert_eq!(http_get(port, "/panic"), Some((500, "Internal Server Error".to_owned())));
    // The server is unaffected.
    assert_eq!(http_get(port, "/ping"), Some((200, "pong".to_owned())));

    cwist::shutdown();
    assert_eq!(finished(&done), Ok(()));
}

#[test]
fn a_deferred_response_is_read_only_for_the_handler_and_middleware() {
    let _serial = serial();
    let port = free_port();
    let seen = Arc::new(Mutex::new(Vec::<String>::new()));
    let handler_seen = Arc::clone(&seen);
    let middleware_seen = Arc::clone(&seen);
    let done = serve(port, move |app| {
        let _ = app.use_middleware(move |req, res, next| {
            next();
            // Middleware is app-wide; only record the deferred route, not
            // the /ping readiness probe.
            if req.path() != Some("/defer") {
                return;
            }
            let late = res.add_header("X-Late", "1");
            middleware_seen.lock().unwrap().push(format!("middleware {late:?}"));
        });
        app.get("/defer", move |req, res| {
            let handle = cwist::AsyncResponse::defer(req, res).expect("defer");
            // Complete on another thread and wait for it, so the completion
            // has written the response while this handler still runs.
            let (tx, rx) = mpsc::channel();
            thread::spawn(move || {
                let _ = tx.send(handle.respond(200, "text/plain", "async body"));
            });
            let responded = rx.recv_timeout(Duration::from_secs(5)).expect("respond");
            let mut seen = handler_seen.lock().unwrap();
            seen.push(format!("responded {responded}"));
            seen.push(format!("deferred {}", res.is_deferred()));
            seen.push(format!("status {}", res.status()));
            res.set_status(418);
            seen.push(format!("body {:?}", res.set_body("late body")));
            seen.push(format!("path {:?}", req.path()));
        })
        .unwrap();
    });

    assert!(wait_until_up(port));
    let response = raw_get(port, "/defer");
    // Stop the server before asserting, so a failure here cannot leave it
    // running for the next test.
    cwist::shutdown();
    assert_eq!(finished(&done), Ok(()));
    assert!(response.starts_with("HTTP/1.1 200"), "{response}");
    assert!(response.ends_with("\r\n\r\nasync body"), "{response}");
    assert!(!response.contains("X-Late"), "{response}");
    assert_eq!(
        *seen.lock().unwrap(),
        [
            "responded true",
            "deferred true",
            "status 0",
            "body Err(Deferred)",
            "path Some(\"/defer\")",
            "middleware Err(Deferred)",
        ]
    );
}

#[test]
fn completing_after_a_timeout_returns_false() {
    let _serial = serial();
    let port = free_port();
    let (tx, rx) = mpsc::channel::<cwist::AsyncResponse>();
    let done = serve(port, move |app| {
        app.get("/slow", move |req, res| {
            let handle = cwist::AsyncResponse::defer(req, res).expect("defer");
            handle.set_timeout(50);
            tx.send(handle).expect("send async handle");
        })
        .unwrap();
    });

    assert!(wait_until_up(port));
    let response = raw_get(port, "/slow");
    assert!(response.starts_with("HTTP/1.1 504"), "{response}");

    // The timeout already answered and released the completion's reference;
    // the handle's own reference keeps it valid, and the late answers lose.
    let handle = rx.recv_timeout(Duration::from_secs(5)).expect("receive async handle");
    let mut owned = cwist::OwnedResponse::new().unwrap();
    owned.set_status(200);
    owned.set_body("too late").unwrap();
    assert!(!handle.respond_with(owned));

    assert_eq!(http_get(port, "/slow").map(|(s, _)| s), Some(504));
    let handle = rx.recv_timeout(Duration::from_secs(5)).expect("receive async handle");
    drop(handle);

    assert_eq!(http_get(port, "/ping"), Some((200, "pong".to_owned())));
    cwist::shutdown();
    assert_eq!(finished(&done), Ok(()));
}
