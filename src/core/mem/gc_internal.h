/**
 * @file gc_internal.h
 * @brief Internal bridge between the allocator and the epoch GC wrapper.
 *
 * Not part of the installed public API: these declarations are shared only
 * between the GC implementation and internal ownership-transfer paths.
 */

#ifndef __CWIST_CORE_MEM_GC_INTERNAL_H__
#define __CWIST_CORE_MEM_GC_INTERNAL_H__

#include <stdbool.h>

/**
 * @brief Detach a pointer previously handed to a GC context through
 *        cwist_reg_ptr()/cwist_reg_ptr_sized(), without freeing it.
 *
 * cwist_free() calls this on every block so an explicit free of a
 * registered pointer is safe (the owning context would otherwise free it
 * again at cwist_gc_shutdown()). For blocks that were never registered it
 * is a cheap no-op.
 *
 * @param ptr Block being freed; a no-op when NULL.
 * @return true when @p ptr was registered and has been detached.
 */
bool cwist_gc_reg_untrack(void *ptr);
bool cwist_conn_registry_disown(void *handle);

#endif
