use cwist_sys as sys;
use std::ffi::CString;
use std::ptr;

/// A deferred HTTP response handle obtained from inside a route handler.
///
/// Calling [`AsyncResponse::defer`] transfers ownership of the request/response
/// pair from the handler to this handle. The handler must return immediately
/// afterwards and must not touch [`Request`](crate::Request) or
/// [`Response`](crate::Response) again.
///
/// The handle is thread-safe and `'static`: it can be moved to a worker thread,
/// a scheduler job, or a NATS callback and completed there.
///
/// Completion is one-shot: the first successful `respond`/`respond_with`/`abort`
/// wins and later calls return `false`.
pub struct AsyncResponse {
    raw: std::ptr::NonNull<sys::cwist_async>,
}

// SAFETY: CWIST documents cwist_async_* as thread-safe and reference counted.
unsafe impl Send for AsyncResponse {}
unsafe impl Sync for AsyncResponse {}

impl AsyncResponse {
    /// Defer the response for the current request.
    ///
    /// Returns `None` if CWIST could not allocate the async handle; in that
    /// case the handler should fall back to writing a response normally.
    ///
    /// # Safety
    ///
    /// The caller must not use `req` or `res` after this call.
    pub fn defer(req: &crate::Request, res: &mut crate::Response) -> Option<AsyncResponse> {
        let raw = unsafe {
            sys::cwist_async_defer(req.raw().as_ptr(), res.raw().as_ptr())
        };
        std::ptr::NonNull::new(raw).map(|raw| AsyncResponse { raw })
    }

    /// Set a timeout in milliseconds. If the exchange is still pending when the
    /// timeout expires, CWIST answers with 504 Gateway Timeout.
    pub fn set_timeout(&self, ms: u64) {
        // SAFETY: self.raw is a live handle; set_timeout only writes a timer.
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
        // Prevent Drop from releasing the handle a second time: respond()
        // consumes the reference owned by this struct.
        std::mem::forget(self);
        ok
    }

    /// Complete the exchange with a caller-built response.
    ///
    /// On success CWIST takes ownership of `response` and destroys it after the
    /// response is sent. Returns `true` if this call was the first to complete
    /// the exchange.
    pub fn respond_with(self, response: crate::OwnedResponse) -> bool {
        let raw = response.raw();
        // SAFETY: self.raw is a live handle; response.raw is a live owned
        // response. Ownership transfers to CWIST, so do not run our Drop.
        let ok = unsafe { sys::cwist_async_respond_with(self.raw.as_ptr(), raw.as_ptr()) };
        std::mem::forget(response);
        std::mem::forget(self);
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
        std::mem::forget(self);
        ok
    }

    /// Acquire an additional reference to the handle.
    ///
    /// Useful when a producer may outlive the original handle, for example
    /// because it is stored in a long-lived callback.
    pub fn retain(&self) {
        // SAFETY: self.raw is a live handle.
        unsafe { sys::cwist_async_retain(self.raw.as_ptr()) };
    }
}

impl Drop for AsyncResponse {
    fn drop(&mut self) {
        // SAFETY: self.raw is a live handle. One defer/retain reference is
        // released here without completing the exchange.
        unsafe { sys::cwist_async_release(self.raw.as_ptr()) };
    }
}
