/* The C API the Zig bindings use, translated by build.zig (addTranslateC)
 * from the installed CWIST headers. Only the canonical headers: the legacy
 * umbrella headers carry stale copies of some structs (tests/abi_layout.h).
 *
 * The system <stdatomic.h> and <pthread.h> come first so that libttak's
 * headers use them instead of their portable fallback (build.zig defines
 * __TTAK_STDATOMIC_SYSTEM_INCLUDED to match). */
#include <stdatomic.h>
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
