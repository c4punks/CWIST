//! Safe Rust API for CWIST, built on the raw bindings in `cwist-sys`.
//!
//! ```no_run
//! use cwist::App;
//!
//! let mut app = App::new()?;
//! app.get("/users/:id", |req, res| {
//!     let id = req.param("id").unwrap_or("?");
//!     res.set_status(200);
//!     let _ = res.set_body(format!("user {id}"));
//! })?;
//! let response = app.dispatch(b"GET /users/7 HTTP/1.1\r\nHost: localhost\r\n\r\n")?;
//! # Ok::<(), cwist::Error>(())
//! ```
//!
//! # Ownership
//!
//! * [`App`] owns the underlying `cwist_app` and destroys it on drop.
//! * Each route handler is a boxed closure handed to CWIST through the
//!   `cwist_app_*_ex` routes as their user context, together with a
//!   destructor. CWIST calls that destructor exactly once: when the app is
//!   destroyed, when the route is registered again, or immediately if
//!   registration fails. Rust never frees a context it has handed over.
//! * [`Request`] and [`Response`] only borrow the C objects for the duration
//!   of one handler call; the handler signature keeps them from escaping.
//!   Buffers CWIST returns to Rust are copied and released with `cwist_free`.
//!
//! # Panics and threads
//!
//! An unwinding panic in a handler is caught at the callback boundary and
//! the request is answered with `500 Internal Server Error`.
//! A panic while a handler is being dropped is caught the same way. Caught
//! panic payloads are intentionally leaked: their destructors can panic again.
//! Aborting panics (`panic = "abort"`) cannot be caught.
//! CWIST may call one handler from several worker threads at once, so
//! handlers must be `Send + Sync`.
//!
//! This crate is experimental (ROADMAP.md, v3.8 Phase 2).

#![deny(unsafe_op_in_unsafe_fn)]
#![warn(missing_docs)]

mod app;
mod error;
mod http;
mod server;

pub use app::App;
pub use error::Error;
pub use http::{Method, Request, Response};
pub use server::shutdown;
