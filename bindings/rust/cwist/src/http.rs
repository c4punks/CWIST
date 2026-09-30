use crate::Error;
use cwist_sys as sys;
use std::ffi::{CStr, CString};
use std::marker::PhantomData;
use std::ptr::NonNull;
use std::slice;

/// HTTP request method.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[non_exhaustive]
pub enum Method {
    /// `GET`
    Get,
    /// `POST`
    Post,
    /// `PUT`
    Put,
    /// `DELETE`
    Delete,
    /// `PATCH`
    Patch,
    /// `HEAD`
    Head,
    /// `OPTIONS`
    Options,
    /// `CONNECT`
    Connect,
    /// A method CWIST did not recognise.
    Unknown,
}

/// Checks a `cwist_error_t` and releases it, as C callers must.
pub(crate) fn consume(mut err: sys::cwist_error_t) -> bool {
    // SAFETY: err is a valid value returned by CWIST; dispose frees only a
    // JSON payload it owns and leaves the struct valid.
    unsafe {
        let ok = sys::cwist_error_is_ok_extern(&err);
        sys::cwist_error_dispose(&mut err);
        ok
    }
}

/// Bytes of a `cwist_sstring`, borrowed for `'a`.
///
/// # Safety
/// `s` is NULL or points to a live sstring whose buffer is not modified
/// or freed during `'a`.
unsafe fn sstring_bytes<'a>(s: *const sys::cwist_sstring) -> &'a [u8] {
    // SAFETY: guaranteed by the caller; data/size describe the buffer.
    unsafe {
        match s.as_ref() {
            Some(s) if !s.data.is_null() && s.size > 0 => {
                slice::from_raw_parts(s.data as *const u8, s.size)
            }
            _ => &[],
        }
    }
}

/// A NUL-terminated C string returned by CWIST, borrowed for `'a`, if it
/// is present and valid UTF-8.
///
/// # Safety
/// `p` is NULL or a NUL-terminated string that stays valid during `'a`.
unsafe fn c_str<'a>(p: *const std::os::raw::c_char) -> Option<&'a str> {
    if p.is_null() {
        return None;
    }
    // SAFETY: guaranteed by the caller.
    unsafe { CStr::from_ptr(p) }.to_str().ok()
}

/// The request passed to a route handler.
///
/// Borrows CWIST's request object for one handler call; values it returns
/// cannot outlive that call.
pub struct Request<'a> {
    raw: NonNull<sys::cwist_http_request>,
    _borrow: PhantomData<&'a sys::cwist_http_request>,
}

impl<'a> Request<'a> {
    /// # Safety
    /// `raw` points to a request that CWIST keeps alive, and that nothing
    /// else modifies, for `'a`.
    pub(crate) unsafe fn from_raw(raw: NonNull<sys::cwist_http_request>) -> Self {
        Request { raw, _borrow: PhantomData }
    }

    pub(crate) fn raw(&self) -> NonNull<sys::cwist_http_request> {
        self.raw
    }

    fn get(&self) -> &sys::cwist_http_request {
        // SAFETY: from_raw's contract.
        unsafe { self.raw.as_ref() }
    }

    /// The request method.
    pub fn method(&self) -> Method {
        match self.get().method {
            m if m == sys::CWIST_HTTP_GET => Method::Get,
            m if m == sys::CWIST_HTTP_POST => Method::Post,
            m if m == sys::CWIST_HTTP_PUT => Method::Put,
            m if m == sys::CWIST_HTTP_DELETE => Method::Delete,
            m if m == sys::CWIST_HTTP_PATCH => Method::Patch,
            m if m == sys::CWIST_HTTP_HEAD => Method::Head,
            m if m == sys::CWIST_HTTP_OPTIONS => Method::Options,
            m if m == sys::CWIST_HTTP_CONNECT => Method::Connect,
            _ => Method::Unknown,
        }
    }

    /// The request path without the query string, e.g. `/users/7`, or
    /// `None` if it is not valid UTF-8.
    pub fn path(&self) -> Option<&'a str> {
        // SAFETY: the path sstring belongs to the request (from_raw's contract).
        std::str::from_utf8(unsafe { sstring_bytes(self.get().path) }).ok()
    }

    /// The request body.
    pub fn body(&self) -> &'a [u8] {
        // SAFETY: the body sstring belongs to the request.
        unsafe { sstring_bytes(self.get().body) }
    }

    /// The value of a request header (case-insensitive name), if present and
    /// valid UTF-8.
    pub fn header(&self, name: &str) -> Option<&'a str> {
        let name = CString::new(name).ok()?;
        // SAFETY: the header list belongs to the request; the returned value
        // is owned by that list.
        unsafe { c_str(sys::cwist_http_header_get(self.get().headers, name.as_ptr())) }
    }

    /// A path parameter captured by a `:name` segment of the route.
    pub fn param(&self, name: &str) -> Option<&'a str> {
        self.map_get(self.get().path_params, name)
    }

    /// A query-string parameter (`?name=value`).
    pub fn query(&self, name: &str) -> Option<&'a str> {
        self.map_get(self.get().query_params, name)
    }

    fn map_get(&self, map: *mut sys::cwist_query_map, name: &str) -> Option<&'a str> {
        if map.is_null() {
            return None;
        }
        let name = CString::new(name).ok()?;
        // SAFETY: the map belongs to the request; the value is owned by it.
        unsafe { c_str(sys::cwist_query_map_get(map, name.as_ptr())) }
    }
}

/// The response a route handler fills in.
///
/// Borrows CWIST's response object for one handler call.
pub struct Response<'a> {
    raw: NonNull<sys::cwist_http_response>,
    _borrow: PhantomData<&'a mut sys::cwist_http_response>,
}

impl<'a> Response<'a> {
    /// # Safety
    /// `raw` points to a response that CWIST keeps alive, and that nothing
    /// else accesses, for `'a`.
    pub(crate) unsafe fn from_raw(raw: NonNull<sys::cwist_http_response>) -> Self {
        Response { raw, _borrow: PhantomData }
    }

    pub(crate) fn raw(&self) -> NonNull<sys::cwist_http_response> {
        self.raw
    }

    /// The response, or `None` once it has been deferred: from then on an
    /// [`AsyncResponse`](crate::AsyncResponse) completion may write it from
    /// another thread, so Rust must not touch it again.
    fn get(&mut self) -> Option<&mut sys::cwist_http_response> {
        if self.is_deferred() {
            return None;
        }
        // SAFETY: from_raw's contract; not deferred, so nothing else writes it.
        Some(unsafe { self.raw.as_mut() })
    }

    /// Whether [`AsyncResponse::defer`](crate::AsyncResponse::defer) took this
    /// response. A deferred response is answered by its `AsyncResponse`:
    /// [`status`](Self::status) returns 0, [`set_status`](Self::set_status)
    /// does nothing, and the other setters return [`Error::Deferred`].
    pub fn is_deferred(&self) -> bool {
        // SAFETY: from_raw's contract. Only cwist_async_defer writes this
        // field, on the thread running the handler (this one); completions on
        // other threads never do, so reading it alone cannot race with them.
        unsafe { std::ptr::addr_of!((*self.raw.as_ptr()).deferred).read() }
    }

    /// The current status code, or 0 once the response has been deferred.
    pub fn status(&self) -> u16 {
        if self.is_deferred() {
            return 0;
        }
        // SAFETY: from_raw's contract; a plain field read of a response that
        // nothing else writes while it is not deferred.
        let code = unsafe { self.raw.as_ref() }.status_code;
        u16::try_from(code).unwrap_or(0)
    }

    /// Sets the status code, e.g. `201`. Does nothing once the response has
    /// been deferred.
    pub fn set_status(&mut self, code: u16) {
        if let Some(res) = self.get() {
            res.status_code = code.into();
        }
    }

    /// Replaces the response body.
    pub fn set_body(&mut self, body: impl AsRef<[u8]>) -> Result<(), Error> {
        let body = body.as_ref();
        let sstr = self.get().ok_or(Error::Deferred)?.body;
        if sstr.is_null() {
            return Err(Error::Body);
        }
        // SAFETY: sstr is the response's own body sstring; CWIST copies the
        // bytes, so `body` only needs to live for the call.
        let err = unsafe { sys::cwist_sstring_assign_len(sstr, body.as_ptr().cast(), body.len()) };
        if consume(err) {
            Ok(())
        } else {
            Err(Error::Body)
        }
    }

    /// Adds a response header. CWIST copies both strings and rejects names or
    /// values containing CR or LF.
    pub fn add_header(&mut self, name: &str, value: &str) -> Result<(), Error> {
        let name = CString::new(name).map_err(|_| Error::InteriorNul("header name"))?;
        let value = CString::new(value).map_err(|_| Error::InteriorNul("header value"))?;
        let headers = &mut self.get().ok_or(Error::Deferred)?.headers;
        // SAFETY: headers is the response's own list head; CWIST copies both
        // strings before returning.
        let err = unsafe { sys::cwist_http_header_add(headers, name.as_ptr(), value.as_ptr()) };
        if consume(err) {
            Ok(())
        } else {
            Err(Error::Header)
        }
    }
}

/// An owned HTTP response allocated by CWIST.
///
/// This is useful when completing a deferred response with a fully constructed
/// response object via [`AsyncResponse::respond_with`](crate::AsyncResponse::respond_with).
/// The response is destroyed automatically when this value is dropped, unless
/// ownership is explicitly transferred to CWIST.
pub struct OwnedResponse {
    raw: NonNull<sys::cwist_http_response>,
}

impl OwnedResponse {
    /// Creates a new empty response, or returns an error if allocation fails.
    pub fn new() -> Result<OwnedResponse, Error> {
        // SAFETY: cwist_http_response_create allocates a response with body and
        // headers initialized; NULL means allocation failed.
        let raw = unsafe { sys::cwist_http_response_create() };
        NonNull::new(raw)
            .map(|raw| OwnedResponse { raw })
            .ok_or(Error::Body)
    }

    pub(crate) fn raw(&self) -> NonNull<sys::cwist_http_response> {
        self.raw
    }

    fn get(&mut self) -> &mut sys::cwist_http_response {
        // SAFETY: self.raw is a live response owned by this struct.
        unsafe { self.raw.as_mut() }
    }

    /// Sets the status code, e.g. `201`.
    pub fn set_status(&mut self, code: u16) {
        self.get().status_code = code.into();
    }

    /// Replaces the response body.
    pub fn set_body(&mut self, body: impl AsRef<[u8]>) -> Result<(), Error> {
        let body = body.as_ref();
        let sstr = self.get().body;
        if sstr.is_null() {
            return Err(Error::Body);
        }
        // SAFETY: sstr is the response's own body sstring; CWIST copies the
        // bytes, so `body` only needs to live for the call.
        let err = unsafe { sys::cwist_sstring_assign_len(sstr, body.as_ptr().cast(), body.len()) };
        if consume(err) {
            Ok(())
        } else {
            Err(Error::Body)
        }
    }

    /// Adds a response header. CWIST copies both strings and rejects names or
    /// values containing CR or LF.
    pub fn add_header(&mut self, name: &str, value: &str) -> Result<(), Error> {
        let name = CString::new(name).map_err(|_| Error::InteriorNul("header name"))?;
        let value = CString::new(value).map_err(|_| Error::InteriorNul("header value"))?;
        let headers = &mut self.get().headers;
        // SAFETY: headers is the response's own list head; CWIST copies both
        // strings before returning.
        let err = unsafe { sys::cwist_http_header_add(headers, name.as_ptr(), value.as_ptr()) };
        if consume(err) {
            Ok(())
        } else {
            Err(Error::Header)
        }
    }
}

impl Drop for OwnedResponse {
    fn drop(&mut self) {
        // SAFETY: self.raw is a live response allocated by CWIST.
        unsafe { sys::cwist_http_response_destroy(self.raw.as_ptr()) };
    }
}
