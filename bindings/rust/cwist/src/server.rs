use crate::Error;
use cwist_sys as sys;
use std::sync::atomic::{AtomicBool, Ordering};

/// Set while an [`App::listen`](crate::App::listen) call is running. CWIST's
/// server state (listening sockets, the running flag, the handler pool) is
/// process-wide, so two servers at once would interfere.
static LISTENING: AtomicBool = AtomicBool::new(false);

/// Held for the duration of one `listen` call.
pub(crate) struct ListenGuard(());

impl ListenGuard {
    pub(crate) fn acquire() -> Result<ListenGuard, Error> {
        LISTENING
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .map(|_| ListenGuard(()))
            .map_err(|_| Error::AlreadyListening)
    }
}

impl Drop for ListenGuard {
    fn drop(&mut self) {
        LISTENING.store(false, Ordering::Release);
    }
}

/// Requests a graceful shutdown of the server running in this process.
///
/// [`App::listen`](crate::App::listen) stops accepting connections, drains,
/// and returns. This has the same effect as sending the process SIGTERM or
/// SIGINT, and like them it is process-wide.
///
/// It can be called from any thread, including from inside a route handler,
/// and calling it more than once is harmless. If no server is running, the
/// request applies to the next `listen`, which then returns at once.
pub fn shutdown() {
    // SAFETY: no preconditions; cwist_shutdown_request only stores an atomic
    // flag and closes the registered listening sockets, each at most once.
    unsafe { sys::cwist_shutdown_request() };
}
