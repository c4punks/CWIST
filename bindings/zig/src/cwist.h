/* The C API the Zig bindings use, translated by build.zig (addTranslateC)
 * from the installed CWIST headers. Only the canonical headers: the legacy
 * umbrella headers carry stale copies of some structs (tests/abi_layout.h).
 *
 * Ensure portable/host <stdatomic.h> shadowing does not conflict with
 * translate-c by defining standard atomic types and neutralizing _Atomic. */
#define _STDATOMIC_H
#define _STDATOMIC_H_
#define __STDATOMIC_H
#define __STDATOMIC_H__
#define TTAK_PORTABLE_STDATOMIC_H
#define __TTAK_STDATOMIC_SYSTEM_INCLUDED

#ifndef _Atomic
#define _Atomic(T) T
#endif
typedef volatile int atomic_int;
typedef volatile _Bool atomic_bool;
typedef volatile unsigned int atomic_uint;
typedef volatile long atomic_long;
typedef volatile unsigned long atomic_ulong;
typedef volatile long long atomic_llong;
typedef volatile unsigned long long atomic_ullong;
typedef volatile __SIZE_TYPE__ atomic_size_t;
typedef volatile __UINTPTR_TYPE__ atomic_uintptr_t;

typedef enum {
    memory_order_relaxed,
    memory_order_consume,
    memory_order_acquire,
    memory_order_release,
    memory_order_acq_rel,
    memory_order_seq_cst
} memory_order;

#include <pthread.h>

#include <cwist/core/mem/alloc.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/net/http/async.h>
#include <cwist/net/http/http.h>
#include <cwist/net/http/query.h>
#include <cwist/sys/app/app.h>
#include <cwist/sys/app/compress.h>
#include <cwist/sys/app/endpoint_opts.h>
#include <cwist/sys/app/middleware.h>
#include <cwist/sys/app/shutdown.h>
#include <cwist/sys/err/cwist_err.h>
