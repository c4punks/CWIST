//! Request-scoped bump allocator wrapping CWIST's `cwist_arena_t`.

use cwist_sys as sys;
use std::ptr::NonNull;
use std::slice;

/// A bump allocator backed by a recycled generation buffer: many small
/// allocations, one bulk release on drop.
///
/// Allocations come from the active generation buffer; when it is exhausted
/// an allocation returns `None` (CWIST's contract: the caller falls back to
/// the heap), so a single allocation never exceeds the generation capacity
/// by much. All memory is released by `cwist_arena_destroy` when the arena
/// is dropped.
///
/// # Safety contract
///
/// Every allocation goes through `&mut self` and hands out references tied
/// to that borrow, so a safe caller can never hold two live mutable views of
/// the same memory and can never touch the arena through `&self` while a
/// mutable allocation is live. `Drop` therefore cannot run while any
/// reference handed out here is alive, and no use-after-free is possible
/// without `unsafe`.
///
/// `Arena` is neither `Send` nor `Sync`: the generation buffer is a plain
/// mutable C object with no internal locking.
pub struct Arena {
    raw: NonNull<sys::cwist_arena_t>,
}

impl Arena {
    /// Creates an arena whose generation buffers hold `generation_bytes`
    /// bytes (0 selects CWIST's default, 8 KiB). Returns `None` on failure.
    pub fn new(generation_bytes: usize) -> Option<Arena> {
        // SAFETY: no preconditions; NULL means allocation failed.
        let raw = unsafe { sys::cwist_arena_create(generation_bytes) };
        NonNull::new(raw).map(|raw| Arena { raw })
    }

    /// Allocates `len` bytes and returns them as a zeroed slice. Returns
    /// `None` when the arena is exhausted (CWIST documents NULL as the
    /// fall-back-to-heap signal). The memory is not zeroed by CWIST, so it
    /// is zeroed here before the safe slice is handed out.
    pub fn alloc_bytes(&mut self, len: usize) -> Option<&mut [u8]> {
        // SAFETY: the arena is live and exclusively borrowed by &mut self.
        let ptr = unsafe { sys::cwist_arena_alloc(self.raw.as_ptr(), len) };
        let ptr = NonNull::new(ptr.cast::<u8>())?;
        // SAFETY: ptr owns a fresh `len`-byte chunk not aliased by any other
        // allocation; the borrow of self keeps the arena alive.
        unsafe {
            std::ptr::write_bytes(ptr.as_ptr(), 0, len);
            Some(slice::from_raw_parts_mut(ptr.as_ptr(), len))
        }
    }

    /// Whether `ptr` lies inside the arena's active generation buffer.
    pub fn owns(&self, ptr: *const u8) -> bool {
        // SAFETY: the arena is live; ptr is only compared, not dereferenced.
        unsafe { sys::cwist_arena_owns(self.raw.as_ptr(), ptr.cast()) }
    }

    /// Allocates room for `count` values of `T` without initializing them
    /// and returns a pointer to the first element. For zero-copy paths where
    /// the caller writes the contents itself.
    ///
    /// # Safety
    ///
    /// * The returned memory is uninitialized: the caller must fully
    ///   initialize it before any read, and must not read it on `None`-less
    ///   paths before writing.
    /// * `T` must not need dropping, or the caller must drop each element
    ///   explicitly before the arena is destroyed: the arena releases the
    ///   memory in bulk on drop and never runs destructors.
    /// * The pointer must not outlive the arena, and must not be written
    ///   through while any safe reference from `alloc_bytes` is live.
    pub unsafe fn alloc_uninit<T>(&mut self, count: usize) -> Option<NonNull<T>> {
        let layout = std::alloc::Layout::new::<T>();
        let len = layout.size().checked_mul(count)?;
        // SAFETY: the arena is live and exclusively borrowed by &mut self.
        let ptr = unsafe { sys::cwist_arena_alloc(self.raw.as_ptr(), len) };
        // CWIST documents the returned chunk as aligned.
        NonNull::new(ptr.cast::<T>())
    }
}

impl Drop for Arena {
    fn drop(&mut self) {
        // SAFETY: the arena is live and owned by this struct; every
        // allocation made from it is released by this call.
        unsafe { sys::cwist_arena_destroy(self.raw.as_ptr()) };
    }
}
