use crate::Error;
use cwist_sys as sys;
use std::sync::atomic::{AtomicBool, Ordering};

/// Set while an [`App::listen`](crate::App::listen) call runs. CWIST's server
/// state is process-wide, so only one server may run at a time.
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

/// Requests a graceful shutdown of the server in this process, like
/// SIGTERM/SIGINT: [`App::listen`](crate::App::listen) stops accepting,
/// drains and returns. Callable from any thread or handler, any number of
/// times; with no server running, the next `listen` returns at once.
pub fn shutdown() {
    // SAFETY: no preconditions; it only sets an atomic flag and shuts down
    // the listening sockets, each at most once.
    unsafe { sys::cwist_shutdown_request() };
}
