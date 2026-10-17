//! Request-scoped bump allocator wrapping CWIST's `cwist_arena_t`.

use cwist_sys as sys;
use std::ptr::NonNull;
use std::slice;

/// A bump allocator backed by a recycled generation buffer: many small
/// allocations, one bulk release on drop.
///
/// An allocation returns `None` when the active generation is exhausted —
/// CWIST's contract is that the caller then falls back to the heap.
///
/// Allocations take `&mut self` and return references tied to that borrow,
/// so `Drop` cannot run while one is live. `Arena` is neither `Send` nor
/// `Sync`: the generation buffer has no internal locking.
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

    /// Allocates `len` bytes and returns them as a zeroed slice (CWIST's
    /// chunks are uninitialized). Returns `None` when the arena is exhausted.
    pub fn alloc_bytes(&mut self, len: usize) -> Option<&mut [u8]> {
        // SAFETY: the arena is live and exclusively borrowed by &mut self.
        let ptr = unsafe { sys::cwist_arena_alloc(self.raw.as_ptr(), len) };
        let ptr = NonNull::new(ptr.cast::<u8>())?;
        // SAFETY: ptr is a fresh `len`-byte chunk aliased by nothing else.
        unsafe {
            std::ptr::write_bytes(ptr.as_ptr(), 0, len);
            Some(slice::from_raw_parts_mut(ptr.as_ptr(), len))
        }
    }

    /// Whether `ptr` lies inside the arena's active generation buffer.
    pub fn owns(&self, ptr: *const u8) -> bool {
        // SAFETY: ptr is only compared, not dereferenced.
        unsafe { sys::cwist_arena_owns(self.raw.as_ptr(), ptr.cast()) }
    }

    /// Allocates room for `count` values of `T` without initializing them,
    /// for zero-copy paths where the caller writes the contents itself.
    ///
    /// # Safety
    ///
    /// The memory is uninitialized and the arena never runs destructors: the
    /// caller must initialize every element before any read, and drop each
    /// one explicitly before the arena is destroyed if `T` needs dropping.
    /// The pointer must not outlive the arena or alias a live allocation.
    pub unsafe fn alloc_uninit<T>(&mut self, count: usize) -> Option<NonNull<T>> {
        let len = std::alloc::Layout::new::<T>().size().checked_mul(count)?;
        // SAFETY: the arena is live and exclusively borrowed by &mut self;
        // CWIST documents the returned chunk as aligned.
        let ptr = unsafe { sys::cwist_arena_alloc(self.raw.as_ptr(), len) };
        NonNull::new(ptr.cast::<T>())
    }
}

impl Drop for Arena {
    fn drop(&mut self) {
        // SAFETY: the arena is live and owned by this struct.
        unsafe { sys::cwist_arena_destroy(self.raw.as_ptr()) };
    }
}
