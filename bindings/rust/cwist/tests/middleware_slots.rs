//! `cwist::middleware::jwt_auth` when CWIST's secret slots are used up.
//!
//! CWIST keeps at most eight distinct JWT secrets per process, so this runs
//! in its own test binary, away from the other JWT tests.

use cwist::{middleware, App, Error};

#[test]
fn jwt_auth_fails_closed_when_the_secret_slots_are_full() {
    const SECRETS: [&str; 8] =
        ["slot-0", "slot-1", "slot-2", "slot-3", "slot-4", "slot-5", "slot-6", "slot-7"];
    for secret in SECRETS {
        middleware::jwt_auth(secret).expect("a free secret slot");
    }
    // A secret that is already registered reuses its slot.
    middleware::jwt_auth("slot-3").expect("the same secret reuses its slot");
    // A ninth distinct secret has nowhere to go: the factory fails instead of
    // handing out a middleware that would leave routes unauthenticated.
    assert_eq!(middleware::jwt_auth("one-too-many").unwrap_err(), Error::Middleware);

    // Middleware from the registered secrets still guards routes.
    let mut app = App::new().unwrap();
    app.use_builtin_middleware(middleware::jwt_auth("slot-0").unwrap()).unwrap();
    app.get("/r", |_req, res| res.set_body("open").unwrap()).unwrap();
    let raw = app.dispatch(b"GET /r HTTP/1.1\r\nHost: localhost\r\n\r\n").unwrap();
    let response = String::from_utf8(raw).unwrap();
    assert!(response.starts_with("HTTP/1.1 401"), "{response}");
}
