/* Headers cwist-sys generates bindings from. Only the canonical headers:
 * the legacy umbrella headers carry stale copies of some structs (see
 * tests/abi_layout.h). What is actually bound is narrowed by the allowlist
 * in build.rs. */
#include <cwist/core/mem/alloc.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/net/http/async.h>
#include <cwist/net/http/http.h>
#include <cwist/net/http/query.h>
#include <cwist/sys/app/app.h>
#include <cwist/sys/app/endpoint_opts.h>
#include <cwist/sys/app/middleware.h>
#include <cwist/sys/app/shutdown.h>
#include <cwist/sys/err/cwist_err.h>
