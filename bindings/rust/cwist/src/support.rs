use std::ffi::c_void;
use std::panic::{self, AssertUnwindSafe};
use std::ptr::NonNull;

/// Runs `f` inside `catch_unwind`, containing any panic at a C callback
/// boundary (extern "C" must not unwind). A caught payload is leaked rather
/// than dropped, because its destructor could panic again outside the catch.
/// Returns `false` when `f` panicked.
pub(crate) fn catch_leak(f: impl FnOnce()) -> bool {
    panic::catch_unwind(AssertUnwindSafe(f))
        .map_err(std::mem::forget)
        .is_ok()
}

/// A buffer CWIST allocated and the caller must release, freed with `free`
/// on drop (`cwist_free`, `cJSON_free`, ...). Using the guard instead of a
/// raw pointer plus a manual free makes every early-return path leak-proof.
pub(crate) struct CBuf {
    ptr: NonNull<c_void>,
    free: unsafe extern "C" fn(*mut c_void),
}

impl CBuf {
    /// Takes ownership of `ptr`, to be released with `free` on drop. Returns
    /// `None` for NULL.
    pub(crate) fn new(ptr: *mut c_void, free: unsafe extern "C" fn(*mut c_void)) -> Option<CBuf> {
        NonNull::new(ptr).map(|ptr| CBuf { ptr, free })
    }

    pub(crate) fn as_ptr(&self) -> *mut c_void {
        self.ptr.as_ptr()
    }
}

impl Drop for CBuf {
    fn drop(&mut self) {
        // SAFETY: ptr came from the C call that owns this buffer and is
        // released exactly once, here, with the free function the caller
        // paired with it.
        unsafe { (self.free)(self.ptr.as_ptr()) };
    }
}

