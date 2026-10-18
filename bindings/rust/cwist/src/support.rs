use std::panic::{self, AssertUnwindSafe};

/// Runs `f` inside `catch_unwind`, containing any panic at a C callback
/// boundary (extern "C" must not unwind). A caught payload is leaked rather
/// than dropped, because its destructor could panic again outside the catch.
/// Returns `false` when `f` panicked.
pub(crate) fn catch_leak(f: impl FnOnce()) -> bool {
    panic::catch_unwind(AssertUnwindSafe(f))
        .map_err(std::mem::forget)
        .is_ok()
}
