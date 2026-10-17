//! Background job scheduler wrapping CWIST's `cwist_scheduler_t`.

use crate::support::catch_leak;
use cwist_sys as sys;
use std::ffi::c_void;
use std::ptr::NonNull;
use std::time::Duration;

type Job = Box<dyn FnOnce() + Send + 'static>;

/// Boxes a job for the C queue.
fn box_job(job: impl FnOnce() + Send + 'static) -> *mut c_void {
    Box::into_raw(Box::new(Box::new(job) as Job)).cast::<c_void>()
}

/// Releases the boxed job when the queue refused it, and reports acceptance.
fn accepted(ok: bool, arg: *mut c_void) -> bool {
    if !ok {
        // The queue refused the job: arg is still ours, release it.
        // SAFETY: arg came from box_job and was not accepted.
        drop(unsafe { Box::from_raw(arg.cast::<Job>()) });
    }
    ok
}

/// A pool of background worker threads with a delayed task queue.
///
/// A panic in a job is caught at the C boundary; the caught payload is
/// leaked, since its destructor could panic again.
///
/// Dropping the scheduler joins the workers: started jobs run to completion,
/// but a job still queued or delayed is freed by CWIST without running, so
/// one boxed closure per unrun job leaks (there is no C cancel/drain API).
///
/// `Scheduler` is `Send` and `Sync`: the queue is mutex-protected and its
/// running flag is atomic.
pub struct Scheduler {
    raw: NonNull<sys::cwist_scheduler_t>,
}

// SAFETY: see the struct documentation: mutex-protected queue, atomic
// running flag, and destroy joins the workers.
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
        let arg = box_job(job);
        // SAFETY: the scheduler is live; arg is a valid Job box owned by the
        // queue on success.
        let ok = unsafe { sys::cwist_scheduler_submit(self.raw.as_ptr(), Some(job_trampoline), arg) };
        accepted(ok, arg)
    }

    /// Runs `job` on a worker after `delay`. Returns `false` on failure, in
    /// which case the job is dropped here. A delayed job that has not fired
    /// when the scheduler is dropped leaks its closure (see the [`Scheduler`]
    /// documentation).
    pub fn schedule(&self, delay: Duration, job: impl FnOnce() + Send + 'static) -> bool {
        let arg = box_job(job);
        // SAFETY: as in spawn.
        let ok = unsafe {
            sys::cwist_scheduler_schedule(
                self.raw.as_ptr(),
                Some(job_trampoline),
                arg,
                delay.as_millis().min(u128::from(u64::MAX)) as u64,
            )
        };
        accepted(ok, arg)
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
        // joins the workers, so no trampoline runs after this returns.
        unsafe { sys::cwist_scheduler_destroy(self.raw.as_ptr()) };
    }
}

/// C entry point for every job: runs the boxed closure, never letting a
/// panic unwind into C.
unsafe extern "C" fn job_trampoline(arg: *mut c_void) {
    if arg.is_null() {
        return;
    }
    catch_leak(|| {
        // SAFETY: arg is the Job box passed to submit/schedule; ownership
        // moves to this call, which drops it after running.
        let job = unsafe { Box::from_raw(arg.cast::<Job>()) };
        job();
    });
}
