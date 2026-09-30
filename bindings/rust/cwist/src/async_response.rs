use cwist_sys as sys;
use std::cell::Cell;
use std::ffi::CString;
use std::ptr::{self, NonNull};

thread_local! {
    /// Set while [`App::dispatch`](crate::App::dispatch) runs handlers on this
    /// thread. That path frees the request and response as soon as the
    /// handler returns, so nothing may defer them.
    static IN_MEMORY_DISPATCH: Cell<bool> = const { Cell::new(false) };
}

/// Marks the current thread as running an in-memory dispatch until dropped.
pub(crate) struct InMemoryDispatch {
    outer: bool,
}

impl InMemoryDispatch {
    pub(crate) fn enter() -> InMemoryDispatch {
        InMemoryDispatch { outer: IN_MEMORY_DISPATCH.with(|f| f.replace(true)) }
    }
}

impl Drop for InMemoryDispatch {
    fn drop(&mut self) {
        IN_MEMORY_DISPATCH.with(|f| f.set(self.outer));
    }
}

/// A deferred HTTP response handle obtained from inside a route handler.
///
/// Calling [`AsyncResponse::defer`] hands the response to this handle. From
/// then on the handler's [`Response`](crate::Response) is read-only: its
/// setters are ignored or return [`Error::Deferred`](crate::Error::Deferred),
/// so a completion running on another thread never races with the handler
/// or with middleware that runs after `next`. The
/// [`Request`](crate::Request) stays readable until the handler returns.
///
/// The handle is thread-safe and `'static`: it can be moved to a worker thread,
/// a scheduler job, or a NATS callback and completed there.
///
/// Completion is one-shot: the first `respond`/`respond_with`/`abort` (or the
/// timeout) wins and later calls return `false`. Dropping a handle that has
/// not been completed answers `500 Internal Server Error` and closes the
/// connection, so a request is never left without a response.
pub struct AsyncResponse {
    raw: NonNull<sys::cwist_async>,
}

// SAFETY: CWIST documents cwist_async_* as thread-safe and reference counted,
// and the handle holds its own reference.
unsafe impl Send for AsyncResponse {}
unsafe impl Sync for AsyncResponse {}

impl AsyncResponse {
    /// Defers the response for the current request.
    ///
    /// Returns `None` if the response was already deferred, if CWIST could
    /// not allocate the async handle, or inside [`App::dispatch`](crate::App::dispatch),
    /// which answers synchronously. In that case the handler should write a
    /// response normally.
    pub fn defer(req: &crate::Request, res: &mut crate::Response) -> Option<AsyncResponse> {
        if IN_MEMORY_DISPATCH.with(Cell::get) {
            return None;
        }
        // SAFETY: req/res are the live objects of the running handler call.
        let raw = unsafe { sys::cwist_async_defer(req.raw().as_ptr(), res.raw().as_ptr()) };
        let raw = NonNull::new(raw)?;
        // The reference cwist_async_defer created belongs to the completion
        // path, which releases it after sending. Take a separate one for this
        // handle, so it stays valid even if a timeout completes the exchange
        // first; the dispatch handoff has not happened yet, as retain requires.
        // SAFETY: raw is a live handle.
        unsafe { sys::cwist_async_retain(raw.as_ptr()) };
        Some(AsyncResponse { raw })
    }

    /// Set a timeout in milliseconds. If the exchange is still pending when the
    /// timeout expires, CWIST answers with 504 Gateway Timeout.
    pub fn set_timeout(&self, ms: u64) {
        // SAFETY: self.raw is a live handle; the timer takes its own reference.
        unsafe { sys::cwist_async_set_timeout(self.raw.as_ptr(), ms) };
    }

    /// Complete the exchange with a simple body response.
    ///
    /// Returns `true` if this call was the first to complete the exchange.
    pub fn respond(self, status: u16, content_type: &str, body: impl AsRef<[u8]>) -> bool {
        let body = body.as_ref();
        let ct = CString::new(content_type).ok();
        let ct_ptr = ct.as_ref().map(|s| s.as_ptr()).unwrap_or(ptr::null());
        // SAFETY: self.raw is a live handle; CWIST copies body and content_type.
        let ok = unsafe {
            sys::cwist_async_respond(
                self.raw.as_ptr(),
                status as sys::cwist_http_status_t,
                ct_ptr,
                body.as_ptr().cast(),
                body.len(),
            )
        };
        self.release();
        ok
    }

    /// Complete the exchange with a caller-built response.
    ///
    /// On success CWIST takes ownership of `response` and destroys it after the
    /// response is sent; otherwise `response` is dropped here. Returns `true`
    /// if this call was the first to complete the exchange.
    pub fn respond_with(self, response: crate::OwnedResponse) -> bool {
        // SAFETY: self.raw is a live handle; response.raw is a live owned
        // response, which CWIST takes only when it returns true.
        let ok =
            unsafe { sys::cwist_async_respond_with(self.raw.as_ptr(), response.raw().as_ptr()) };
        if ok {
            std::mem::forget(response);
        }
        self.release();
        ok
    }

    /// Complete the exchange with an error status and close the connection.
    ///
    /// Returns `true` if this call was the first to complete the exchange.
    pub fn abort(self, status: u16) -> bool {
        // SAFETY: self.raw is a live handle.
        let ok = unsafe {
            sys::cwist_async_abort(self.raw.as_ptr(), status as sys::cwist_http_status_t)
        };
        self.release();
        ok
    }

    /// Releases this handle's reference without completing the exchange.
    fn release(self) {
        let raw = self.raw;
        std::mem::forget(self);
        // SAFETY: the reference taken in defer, released exactly once.
        unsafe { sys::cwist_async_release(raw.as_ptr()) };
    }
}

impl Drop for AsyncResponse {
    fn drop(&mut self) {
        // Not completed through respond/respond_with/abort: answer 500 so the
        // exchange cannot hang (a no-op if the timeout already answered), then
        // release this handle's reference.
        // SAFETY: self.raw is live until the release below, which drops the
        // reference taken in defer exactly once.
        unsafe {
            sys::cwist_async_abort(self.raw.as_ptr(), sys::CWIST_HTTP_INTERNAL_ERROR);
            sys::cwist_async_release(self.raw.as_ptr());
        }
    }
}
