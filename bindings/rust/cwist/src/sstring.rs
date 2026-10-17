//! Owned wrapper for CWIST's `cwist_sstring`.

use crate::http::consume;
use crate::Error;
use cwist_sys as sys;
use std::ptr::NonNull;
use std::slice;

/// A growable string owned by CWIST, destroyed on drop.
///
/// `assign` and `append` copy their input; any `&str` is accepted, embedded
/// NUL bytes included. `SString` is neither `Send` nor `Sync`: the C object
/// has no internal locking.
pub struct SString {
    raw: NonNull<sys::cwist_sstring>,
}

impl SString {
    /// Creates an empty string, or returns `None` if allocation failed.
    pub fn new() -> Option<SString> {
        // SAFETY: no preconditions; NULL means allocation failed.
        let raw = unsafe { sys::cwist_sstring_create() };
        NonNull::new(raw).map(|raw| SString { raw })
    }

    fn get(&self) -> &sys::cwist_sstring {
        // SAFETY: self.raw is a live sstring owned by this struct.
        unsafe { self.raw.as_ref() }
    }

    /// Replaces the contents with a copy of `s`.
    pub fn assign(&mut self, s: &str) -> Result<(), Error> {
        // SAFETY: the sstring is live; CWIST copies the bytes.
        let err = unsafe {
            sys::cwist_sstring_assign_len(self.raw.as_ptr(), s.as_ptr().cast(), s.len())
        };
        if consume(err) {
            Ok(())
        } else {
            Err(Error::SString)
        }
    }

    /// Appends a copy of `s` to the contents.
    pub fn append(&mut self, s: &str) -> Result<(), Error> {
        // SAFETY: as in assign.
        let err = unsafe {
            sys::cwist_sstring_append_len(self.raw.as_ptr(), s.as_ptr().cast(), s.len())
        };
        if consume(err) {
            Ok(())
        } else {
            Err(Error::SString)
        }
    }

    /// The current contents.
    pub fn as_bytes(&self) -> &[u8] {
        let s = self.get();
        if s.data.is_null() {
            return &[];
        }
        // SAFETY: data/size describe the sstring's own live buffer.
        unsafe { slice::from_raw_parts(s.data as *const u8, s.size) }
    }

    /// The current contents as UTF-8, if valid.
    pub fn as_str(&self) -> Option<&str> {
        std::str::from_utf8(self.as_bytes()).ok()
    }
}

impl Default for SString {
    fn default() -> Self {
        SString::new().expect("cwist_sstring_create failed")
    }
}

impl Drop for SString {
    fn drop(&mut self) {
        // SAFETY: the sstring is live and owned by this struct.
        unsafe { sys::cwist_sstring_destroy(self.raw.as_ptr()) };
    }
}
