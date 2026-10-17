//! Background job scheduler wrapping CWIST's `cwist_scheduler_t`.

use cwist_sys as sys;
use std::ffi::c_void;
use std::panic::{self, AssertUnwindSafe};
use std::ptr::NonNull;
use std::time::Duration;

type Job = Box<dyn FnOnce() + Send + 'static>;

/// A pool of background worker threads with a delayed task queue.
///
/// Jobs are boxed closures handed to CWIST as a raw pointer; the worker that
/// runs the job reconstructs the box, runs it, and drops it. A panic in a
/// job is caught at the C boundary and never unwinds into CWIST; the caught
/// payload is intentionally leaked because its destructor can panic again.
/// Aborting panics (`panic = "abort"`) cannot be caught.
///
/// Dropping the scheduler stops the worker threads; jobs they already
/// started run to completion because destroy joins them. A job that is
/// still queued, or delayed and not yet fired, is freed by CWIST *without*
/// being run: its `arg` is released by the C destroy path, so one boxed
/// closure per unrun job leaks (memory leak only, no undefined behaviour).
/// There is no C cancel/drain API to recover them.
///
/// `Scheduler` is `Send` and `Sync`: CWIST documents the scheduler as the
/// background task queue for request handlers, its queue is protected by a
/// mutex and its `running` flag is atomic, so `spawn`, `schedule` and
/// `pending_count` may be called from any thread.
pub struct Scheduler {
    raw: NonNull<sys::cwist_scheduler_t>,
}

// SAFETY: cwist_scheduler_submit/schedule/pending_count take the handle from
// any thread (mutex-protected queue, atomic running flag), and destroy joins
// the workers, so the handle is safe to move and share across threads.
unsafe impl Send for Scheduler {}
unsafe impl Sync for Scheduler {}

impl Scheduler {
    /// Creates a scheduler with `worker_count` worker threads and a queue of
    /// `capacity` entries. Returns `None` if the workers or the queue could
    /// not be created.
    pub fn new(worker_count: usize, capacity: usize) -> Option<Scheduler> {
        // SAFETY: no preconditions; NULL means creation failed.
        let raw = unsafe { sys::cwist_scheduler_create(worker_count, capacity) };
        NonNull::new(raw).map(|raw| Scheduler { raw })
    }

    /// Runs `job` on a worker as soon as one is available. Returns `false`
    /// when the scheduler is stopping or out of memory; in that case the job
    /// is dropped here.
    pub fn spawn(&self, job: impl FnOnce() + Send + 'static) -> bool {
        let arg = Box::into_raw(Box::new(Box::new(job) as Job)).cast::<c_void>();
        // SAFETY: the scheduler is live; arg is a valid Job box owned by the
        // queue on success.
        let ok = unsafe { sys::cwist_scheduler_submit(self.raw.as_ptr(), Some(job_trampoline), arg) };
        if !ok {
            // The queue refused the job: arg is still ours, release it.
            // SAFETY: arg came from Box::into_raw above and was not accepted.
            drop(unsafe { Box::from_raw(arg.cast::<Job>()) });
        }
        ok
    }

    /// Runs `job` on a worker after `delay`. Returns `false` on failure, in
    /// which case the job is dropped here. A delayed job that has not fired
    /// when the scheduler is dropped leaks its closure (see the [`Scheduler`]
    /// documentation).
    pub fn schedule(&self, delay: Duration, job: impl FnOnce() + Send + 'static) -> bool {
        let arg = Box::into_raw(Box::new(Box::new(job) as Job)).cast::<c_void>();
        // SAFETY: as in spawn.
        let ok = unsafe {
            sys::cwist_scheduler_schedule(
                self.raw.as_ptr(),
                Some(job_trampoline),
                arg,
                delay.as_millis().min(u128::from(u64::MAX)) as u64,
            )
        };
        if !ok {
            // SAFETY: as in spawn.
            drop(unsafe { Box::from_raw(arg.cast::<Job>()) });
        }
        ok
    }

    /// The number of delayed jobs currently waiting to fire.
    pub fn pending_count(&self) -> usize {
        // SAFETY: the scheduler is live.
        unsafe { sys::cwist_scheduler_pending_count(self.raw.as_ptr()) }
    }
}

impl Drop for Scheduler {
    fn drop(&mut self) {
        // SAFETY: the scheduler is live and owned by this struct; destroy
        // joins the workers, so no trampoline runs after this returns. Any
        // job still queued or delayed leaks its boxed closure (C-side
        // behaviour; see the struct documentation).
        unsafe { sys::cwist_scheduler_destroy(self.raw.as_ptr()) };
    }
}

/// C entry point for every job: runs the boxed closure, never letting a
/// panic unwind into C.
unsafe extern "C" fn job_trampoline(arg: *mut c_void) {
    if arg.is_null() {
        return;
    }
    let outcome = panic::catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: arg is the Job box passed to submit/schedule; ownership
        // moves to this call, which drops it after running.
        let job = unsafe { Box::from_raw(arg.cast::<Job>()) };
        job();
    }));
    // The payload's own Drop could panic again; leak it instead.
    if let Err(payload) = outcome {
        std::mem::forget(payload);
    }
}
