CC ?= gcc
CXX ?= g++
FUZZ_CC ?= clang

CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
NGHTTP2_CFLAGS := $(shell pkg-config --cflags libnghttp2 2>/dev/null)
BROTLI_CFLAGS := $(shell pkg-config --cflags libbrotlienc libbrotlicommon libbrotlidec 2>/dev/null)

INCLUDE_PATHS = -I./include -I./lib -I./lib/libttak/include -I./lib/cjson -I./lib/sqlite3 -I./lib/uriparser/include -I./lib/cnats/src -I./lib/boringssl/include -I./lib/lsquic/include -I./lib/multipart-parser-c $(CURL_CFLAGS) $(NGHTTP2_CFLAGS) $(BROTLI_CFLAGS)
COMMON_DEFINES = -D_GNU_SOURCE -D_XOPEN_SOURCE=700 -D_REENTRANT -DSQLITE_ENABLE_DESERIALIZE
COMMON_WARNINGS = -std=c17 -Wall -pthread -fPIC
COMMON_CFLAGS = $(INCLUDE_PATHS) $(COMMON_WARNINGS) $(COMMON_DEFINES)

UNAME_S := $(shell uname -s)
CC_VERSION_OUTPUT := $(shell $(CC) --version 2>/dev/null)
IS_CLANG := $(strip $(findstring clang,$(notdir $(CC))) $(findstring Clang,$(CC_VERSION_OUTPUT)) $(findstring clang,$(CC_VERSION_OUTPUT)))
ifneq (,$(IS_CLANG))
    ifeq ($(origin CXX),default)
        CXX = clang++
    endif
endif

# Probe flag support instead of trusting compiler detection: Apple Clang
# reaches the GCC flag block (its --version says "Apple clang") and lacks
# -ffat-lto-objects, so only enable LTO where the flag actually compiles.
SUPPORTS_FAT_LTO := $(shell $(CC) -ffat-lto-objects -x c -c /dev/null -o /dev/null 2>/dev/null && echo 1)

BUILD_PROFILE = perf
ifneq (,$(findstring tcc,$(notdir $(CC))))
    BUILD_PROFILE = tcc
endif

TCC_STACK_FLAGS = -O3 -g \
                  -fno-inline \
                  -fno-omit-frame-pointer \
                  -fno-optimize-sibling-calls \
                  -fno-semantic-interposition \
                  -fno-trapping-math \
                  -falign-functions=32 \
                  -fno-plt \
                  -fno-math-errno

PERF_WARNINGS = -Wextra

# Clang-specific aggressive optimizations (Apple Clang and LLVM Clang compatible)
CLANG_STACK_FLAGS = -O3 -ffast-math -g \
                    -falign-functions=32 \
                    -fomit-frame-pointer \
                    -finline-functions \
                    -fvectorize \
                    -fslp-vectorize \
                    -fstrict-aliasing \
                    -funroll-loops
ifeq ($(UNAME_S),Linux)
    CLANG_STACK_FLAGS += -fno-plt -fno-semantic-interposition
endif

# GCC-specific aggressive optimizations
# -flto=auto lets the final link inline across TUs (event loop <-> handlers);
# -ffat-lto-objects keeps plain code in the objects so a link without -flto
# (or a consumer that just unpacks libcwist.a) still works unchanged.
# Only probed-in (see SUPPORTS_FAT_LTO): Apple Clang lacks the fat flag.
GCC_STACK_FLAGS = -Ofast -g \
                  $(if $(SUPPORTS_FAT_LTO),-flto=auto -ffat-lto-objects) \
                  -fno-plt \
                  -falign-functions=32 \
                  -falign-loops=32 \
                  -falign-jumps=32 \
                  -falign-labels=32 \
                  -fno-semantic-interposition \
                  -fomit-frame-pointer \
                  -finline-functions \
                  -fstrict-aliasing \
                  -funroll-loops

ifeq ($(BUILD_PROFILE),tcc)
    CFLAGS = $(COMMON_CFLAGS) $(TCC_STACK_FLAGS) -ftls-model=global-dynamic
else ifneq (,$(IS_CLANG))
    CFLAGS = $(COMMON_CFLAGS) $(PERF_WARNINGS) $(CLANG_STACK_FLAGS)
else
    CFLAGS = $(COMMON_CFLAGS) $(PERF_WARNINGS) $(GCC_STACK_FLAGS)
endif

URIPARSER_DIR = lib/uriparser
URIPARSER_BUILD_DIR = $(URIPARSER_DIR)/build
URIPARSER_LIB = $(URIPARSER_BUILD_DIR)/liburiparser.a
URIPARSER_CMAKE_FLAGS = -DCMAKE_BUILD_TYPE=Release \
                        -DBUILD_SHARED_LIBS=OFF \
                        -DURIPARSER_SHARED_LIBS=OFF \
                        -DURIPARSER_BUILD_DOCS=OFF \
                        -DURIPARSER_BUILD_TESTS=OFF \
                        -DURIPARSER_BUILD_FUZZERS=OFF \
                        -DURIPARSER_BUILD_TOOLS=OFF

BORINGSSL_DIR = lib/boringssl
BORINGSSL_BUILD_DIR = $(BORINGSSL_DIR)/build
BORINGSSL_SSL_LIB = $(BORINGSSL_BUILD_DIR)/libssl.a
BORINGSSL_CRYPTO_LIB = $(BORINGSSL_BUILD_DIR)/libcrypto.a

LSQUIC_DIR = lib/lsquic
LSQUIC_BUILD_DIR = $(LSQUIC_DIR)/build
LSQUIC_LIB = $(LSQUIC_BUILD_DIR)/src/liblsquic/liblsquic.a

CURL_LIBS := $(shell pkg-config --libs libcurl 2>/dev/null)
NGHTTP2_LIBS := $(shell pkg-config --libs libnghttp2 2>/dev/null)
BROTLI_LIBS := $(shell pkg-config --libs libbrotlienc libbrotlicommon libbrotlidec 2>/dev/null)
# zstd via pkg-config so Homebrew's non-standard lib path (-L/opt/homebrew/...)
# is picked up on macOS; fall back to a bare -lzstd elsewhere.
ZSTD_LIBS := $(shell pkg-config --libs libzstd 2>/dev/null)
ifeq ($(strip $(ZSTD_LIBS)),)
ZSTD_LIBS = -lzstd
endif

LIBS = $(CNATS_LIB) \
       $(LIBTTAK_LIB) \
       $(CJSON_LIB) \
       $(URIPARSER_LIB) \
       $(LSQUIC_LIB) \
       $(BORINGSSL_SSL_LIB) \
       $(BORINGSSL_CRYPTO_LIB) \
       $(CURL_LIBS) \
       $(NGHTTP2_LIBS) \
       $(BROTLI_LIBS) \
       -pthread -ldl -lm -lstdc++ -lz $(ZSTD_LIBS)

# SQLite Automation
SQLITE_YEAR = 2026
SQLITE_VER = 3530400
SQLITE_ZIP = sqlite-amalgamation-$(SQLITE_VER).zip
SQLITE_URL = https://www.sqlite.org/$(SQLITE_YEAR)/$(SQLITE_ZIP)
SQLITE_DIR = lib/sqlite3

# Detect OS
IO_SRC = src/sys/io/io_select.c # Default fallback

ifeq ($(UNAME_S),Linux)
    CFLAGS += -DCWIST_OS_LINUX
    # Hardening: stack canaries on risky frames, libc call checking, and
    # full RELRO + PIE on linked executables.  -U first so toolchains that
    # predefine _FORTIFY_SOURCE do not warn about the redefinition.
    CFLAGS += -fstack-protector-strong -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2
    LIBS += -Wl,-z,relro,-z,now -pie
    # io_queue.c is a lock-free job queue (unrelated to io_uring despite the history).
    IO_SRC = src/sys/io/io_queue.c
endif
ifeq ($(UNAME_S),Darwin)
    # _DARWIN_C_SOURCE: in-file strict _POSIX_C_SOURCE/_XOPEN_SOURCE would
    # otherwise hide BSD types (u_int, ...) and kqueue/sysctl declarations.
    CFLAGS += -DCWIST_OS_BSD -D_DARWIN_C_SOURCE
    IO_SRC = src/sys/io/kqueue.c
endif
ifeq ($(UNAME_S),FreeBSD)
    # _DEFAULT_SOURCE keeps BSD-visible types available under strict
    # _POSIX_C_SOURCE on FreeBSD (same class of issue as on Darwin).
    CFLAGS += -DCWIST_OS_BSD -D_DEFAULT_SOURCE
    IO_SRC = src/sys/io/kqueue.c
endif

# Sanitizer toggle: `make SANITIZE=address,undefined test check` builds the
# library and every test with the given sanitizers.  -fno-lto must come last
# so it wins over the LTO flags baked into the optimization profiles.
ifdef SANITIZE
    CFLAGS += -fsanitize=$(SANITIZE) -fno-lto -fno-omit-frame-pointer
    LIBS += -fsanitize=$(SANITIZE)
endif

# Werror toggle for CI: `make WERROR=1` turns every warning into an error.
# Vendored sources (multipart-parser-c) are exempt; they are not our code.
ifdef WERROR
    CFLAGS += -Werror
endif

lib/multipart-parser-c/multipart_parser.o: CFLAGS := $(filter-out -Werror,$(CFLAGS))
# sqlite3 also skips LTO: lto1 re-optimizing the amalgamation at link time
# trips a known -Wstringop-overread false positive (sqlite3Strlen30) that
# -Werror then promotes to a build failure.
lib/sqlite3/sqlite3.o: CFLAGS := $(filter-out -Werror -flto=auto -ffat-lto-objects,$(CFLAGS))

# Source Files
SRCS = src/core/sstring/sstring.c \
       src/core/seq/seq.c \
       src/core/seq/seq_auth.c \
       src/sys/err/error.c \
       src/net/http/http.c \
       src/net/http/sse.c \
       src/net/graphql/graphql.c \
       src/net/http/http2.c \
       src/net/http/http2_flow_control.c \
       src/net/http/http3.c \
       src/net/http/http3_client.c \
       src/net/http/curl_global.c \
       src/net/http/http_client.c \
       src/net/http/https.c \
       src/net/http/https_upgrade_hook.c \
       src/net/http/tls_chain.c \
       src/net/grpc/grpc.c \
       src/net/grpc/grpc_client.c \
       src/net/grpc/grpc_channel.c \
       src/net/grpc/protobuf.c \
       src/https/pqc_layer.c \
       src/net/http/mux.c \
       src/net/http/multipart.c \
       src/net/http/writer_fast.c \
       src/sys/io/uring_sqpoll.c \
       src/net/http/async_server.c \
       src/net/http/async.c lib/libttak/src/mem/epoch.c src/sys/sys_info.c \
    lib/libttak/src/mem/mem.c lib/libttak/src/mem/fastpath.c \
    lib/libttak/src/mem/owner.c lib/libttak/src/mem/abstract.c \
       src/net/http/cookie.c \
       src/net/http/session.c \
       src/net/http/query.c \
       lib/multipart-parser-c/multipart_parser.c \
       src/sys/session/session_manager.c \
       src/sys/app/csrf.c \
       src/sys/app/waf.c \
       src/core/siphash/siphash.c \
       src/core/db/db.c \
       src/core/db/pool.c \
       src/core/db/nuke_db.c \
       src/core/db/migrate.c \
       src/core/orm/orm.c \
       src/core/orm/orm_socket.c \
       src/core/orm/rdbms_auto_mount.c \
       src/sys/app/app.c \
       src/net/websocket/websocket.c \
       src/net/websocket/websocket_async.c \
       src/net/websocket/ws_utils.c \
       src/core/utils/json_builder.c \
       src/core/utils/json_heal.c \
       src/core/utils/zod.c \
       src/sys/app/middleware.c \
       src/sys/app/config.c \
       src/sys/app/logger.c \
       src/sys/app/shutdown.c \
       src/sys/app/compress.c \
       src/sys/app/test_client.c \
       src/core/log/log.c lib/libttak/src/net/mols_control.c \
    src/net/http/async.c lib/libttak/src/mem/epoch.c src/sys/sys_info.c \
    lib/libttak/src/mem/mem.c lib/libttak/src/mem/fastpath.c \
    lib/libttak/src/mem/owner.c lib/libttak/src/mem/abstract.c \
       src/sys/session/flash.c \
       src/core/template/template.c \
       src/core/html/builder.c \
       src/core/html/css_composer.c \
       src/core/html/component.c \
       src/net/http/html_response.c \
       src/sys/app/assets.c \
       src/sys/app/big_dumb_reply.c \
       src/sys/sys_info.c \
       src/core/mem/alloc.c \
       src/core/mem/arena.c \
       src/core/mem/gc.c \
       lib/sqlite3/sqlite3.c \
       src/security/jwt/jwt.c \
       src/security/db_crypt/db_crypt.c \
       src/security/tls/ech.c \
       src/net/db_sync/db_sync.c \
       src/net/nats/cwist_nats.c \
       src/net/redis/cwist_redis.c \
       src/core/validation/bind.c \
       src/sys/io/reactor.c \
       src/sys/job/scheduler.c \
       src/sys/metrics/metrics.c \
       src/sys/health/healthz.c \
       $(IO_SRC)

# --- WASM (Emscripten) static library -------------------------------------
# Socket-independent core only: app dispatch, mux/middleware, HTTP/1
# parser/serializer, sstring/arena/mem, query, JSON, template, validation.
# Excluded: all of src/sys/io, sockets/accept, TLS/BoringSSL, QUIC/HTTP/3,
# gRPC (needs HTTP/2), WebSocket transport, threads/scheduler, compression,
# database/sync clients.  Build with e.g.
#   make wasm EMCC=/workspace/emsdk/upstream/emscripten/emcc
# The wasm section sits above `all` in this file; without an explicit default
# goal, bare `make` would try to build the wasm archive with emcc.
.DEFAULT_GOAL := all

EMCC ?= emcc
EMAR ?= emar
NODE ?= node
WASM_BUILD_DIR = .wasm-build
WASM_SRCS = src/core/sstring/sstring.c \
       src/core/seq/seq.c \
       src/core/seq/seq_auth.c \
       src/sys/err/error.c \
       src/net/http/http.c \
       src/net/http/mux.c \
       src/net/http/query.c \
       src/net/http/cookie.c \
       src/net/http/session.c \
       src/sys/app/app.c \
       src/sys/app/middleware.c \
       src/sys/app/config.c \
       src/sys/app/logger.c \
       src/sys/app/shutdown.c \
       src/sys/app/big_dumb_reply.c \
       src/sys/app/test_client.c \
       src/core/siphash/siphash.c \
       src/core/utils/json_builder.c \
       src/core/utils/json_heal.c \
       src/core/utils/zod.c \
       src/core/template/template.c \
       src/core/html/builder.c \
       src/core/html/css_composer.c \
       src/core/html/component.c \
       src/net/http/html_response.c \
       src/sys/app/assets.c \
       src/core/validation/bind.c \
       src/core/mem/alloc.c \
       src/core/mem/arena.c \
       src/core/mem/gc.c \
       src/core/db/db.c \
       lib/sqlite3/sqlite3.c \
       lib/cjson/cJSON.c
WASM_OBJS = $(WASM_SRCS:%.c=$(WASM_BUILD_DIR)/%.o)
# Host pkg-config -I paths (curl/nghttp2/...) must NOT leak into the
# emscripten sysroot, so the WASM build uses its own minimal include set.
WASM_INCLUDE_PATHS = -I./include -I./lib -I./lib/cjson -I./lib/boringssl/include -I./lib/libttak/include -I./lib/sqlite3
WASM_CFLAGS = -std=c17 -O2 -Wall $(WASM_INCLUDE_PATHS) $(COMMON_DEFINES)

$(WASM_BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(EMCC) $(WASM_CFLAGS) -c -o $@ $<

libcwist_wasm.a: $(WASM_OBJS)
	$(EMAR) rcs $@ $(WASM_OBJS)

wasm: libcwist_wasm.a

# Manual smoke test (requires Emscripten + node; intentionally not part of
# `make test` since CI has no Emscripten toolchain). NODE is overridable so
# CI can point at a specific node binary.
wasm-smoke: libcwist_wasm.a
	$(EMCC) $(WASM_CFLAGS) -o wasm_smoke.js tests/wasm_smoke.c libcwist_wasm.a
	$(NODE) wasm_smoke.js

# Integration test for the cwist-wasm JS wrapper (wasm/npm): builds a
# consumer-style module through include/cwist/wasm/wasm_entry.h, then drives
# it from node via the wrapper with no Emscripten glue on the JS side.
# Requires Emscripten + node, same as wasm-smoke.
# _main is in EXPORTED_FUNCTIONS on purpose: without it the linker dead-code
# eliminates main() (nothing else references it), and the app would never be
# created - dispatch would return NULL on every call.
wasm-wrapper-test: libcwist_wasm.a
	$(EMCC) $(WASM_CFLAGS) -o wrapper_test.js tests/wasm_wrapper_test.c libcwist_wasm.a \
	    -sEXPORTED_FUNCTIONS=_main,_cwist_wasm_dispatch,_cwist_wasm_dispose,_cwist_wasm_use_session,_malloc,_free \
	    -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32 -sMODULARIZE -sEXPORT_NAME=createCwistModule
	$(NODE) tests/wasm_wrapper_test.js

# Versioned npm-package tarball for the cwist-wasm JS wrapper (issue #93
# Phase 2a). Packs wasm/npm into dist/cwist-wasm-<version>.tgz; CI installs
# the tarball into a clean directory to prove it stands alone.
wasm-dist: wasm-wrapper-test
	mkdir -p dist
	npm --prefix wasm/npm pkg set version=$(VERSION) >/dev/null
	npm pack ./wasm/npm --pack-destination dist >/dev/null
	git checkout -- wasm/npm/package.json 2>/dev/null || true
	@echo "dist/$$(ls dist | grep cwist-wasm | tail -1)"

clean-wasm:
	rm -rf $(WASM_BUILD_DIR) libcwist_wasm.a wasm_smoke.js wasm_smoke.wasm \
	    wrapper_test.js wrapper_test.wasm dist

# --- WASI 0.2 (wasm32-wasip2) smoke ------------------------------------------------
# Same WASM_SRCS subset as the Emscripten target, built with wasi-sdk and run
# under a WASI host (wasmtime). The preview1 target was retired: 0.2 covers
# its use, and three WASM flavors cost more than they earn (issue #203).
# WASI_SDK / WASMTIME are shared with the component targets below.
WASI_SDK ?= $(HOME)/toolchains/wasi-sdk-25.0-x86_64-linux
WASMTIME ?= wasmtime
# Overridable to wasm32-wasip3 for the WASI 0.3 pipeline (issue #203): the
# whole section builds and links unchanged, and wasip3 runs under wasmtime
# 48+. See docs/api/wasm-component.md for the current runtime caveats.
WASIP2_TARGET = wasm32-wasip2
# wasi-sdk ships libwasi-emulated-pthread only for the p1/p2 sysroots; the
# wasip3 libc covers the pthread symbols itself (verified: no undefined
# symbols with the library dropped).
WASIP2_LDLIBS = $(if $(findstring wasip2,$(WASIP2_TARGET)),-lwasi-emulated-pthread,) \
	-lwasi-emulated-getpid
WASIP2_BUILD_DIR = .wasip2-build/$(WASIP2_TARGET)
WASIP2_ARCHIVE = libcwist_wasip2_$(WASIP2_TARGET).a
WASIP2_CFLAGS = -std=c17 -O2 -Wall -fvisibility=hidden \
	-D_WASI_EMULATED_GETPID \
	$(WASM_INCLUDE_PATHS) $(COMMON_DEFINES)
WASIP2_EXTRA_SRCS = src/sys/wasi/compat.c src/sys/metrics/metrics.c \
    src/net/http/writer_fast.c lib/libttak/src/net/lattice.c \
    lib/libttak/src/shared/shared.c lib/libttak/src/timing/deadline.c \
    src/core/log/log.c lib/libttak/src/net/mols_control.c \
    src/net/http/async.c lib/libttak/src/mem/epoch.c src/sys/sys_info.c \
    lib/libttak/src/mem/mem.c lib/libttak/src/mem/fastpath.c \
    lib/libttak/src/mem/owner.c lib/libttak/src/mem/abstract.c
WASIP2_SRCS = $(WASM_SRCS) $(WASIP2_EXTRA_SRCS)
WASIP2_OBJS = $(WASIP2_SRCS:%.c=$(WASIP2_BUILD_DIR)/%.o)
WASIP2_PORT ?= 18099
# The socket request path (16KB read buffer + parse/route/serialize frames +
# sqlite) peaks near 96KB of stack; wasm-ld's 64KB default overflows into
# linear memory and silently corrupts adjacent objects (under wasip3 the first
# request after accept traps with an OOB read at a wild negative SP; see
# docs/api/wasm-component.md "Resolved: socket request path under wasip3").
WASIP2_STACK_BYTES ?= 1048576

$(WASIP2_BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(WASI_SDK)/bin/clang --target=$(WASIP2_TARGET) $(WASIP2_CFLAGS) -c -o $@ $<

$(WASIP2_ARCHIVE): $(WASIP2_OBJS)
	$(WASI_SDK)/bin/ar rcs $@ $(WASIP2_OBJS)

wasip2-smoke: $(WASIP2_ARCHIVE)
	$(WASI_SDK)/bin/clang --target=$(WASIP2_TARGET) $(WASIP2_CFLAGS) \
	    -DWASIP2_SMOKE_PORT=$(WASIP2_PORT) -o wasip2_smoke.wasm tests/wasip2_smoke.c \
	    $(WASIP2_ARCHIVE) $(WASIP2_LDLIBS) \
	    -Wl,--gc-sections -Wl,--allow-undefined -Wl,-z,stack-size=$(WASIP2_STACK_BYTES)
	@set -e; \
	LOG=/tmp/cwist_wasip2_smoke.$$$$.log; \
	if [ "$(findstring wasip3,$(WASIP2_TARGET))" = "" ]; then PREVIEW2="-S preview2=y"; else PREVIEW2=""; fi; \
	$(WASMTIME) run $$PREVIEW2 -S tcp=y -S inherit-network=y \
	    --env CWIST_C1M_MODE=0 wasip2_smoke.wasm >$$LOG 2>&1 & \
	WPID=$$!; \
	trap "kill -9 $$WPID 2>/dev/null || true" EXIT; \
	ok=0; \
	for i in $$(seq 1 30); do \
	    sleep 0.3; \
	    body=$$(curl -s -m 2 http://127.0.0.1:$(WASIP2_PORT)/hello || true); \
	    if [ "$$body" = "hello from WASI 0.2" ]; then ok=1; break; fi; \
	done; \
	cat $$LOG; rm -f $$LOG; \
	if [ $$ok -ne 1 ]; then echo "wasip2-smoke: curl probe failed"; exit 1; fi; \
	kill -9 $$WPID 2>/dev/null || true; \
	echo "wasip2-smoke: PASS (socket server served /hello over wasi:sockets)"

clean-wasip2:
	rm -rf .wasip2-build libcwist_wasip2_*.a wasip2_smoke.wasm

# --- Component boundary (experimental, issue #203) --------------------------------
# wit/cwist.wit is the component-model counterpart of wasm_entry.h. Both
# worlds are validated wherever wit-bindgen is installed; a loud no-op
# elsewhere. CI pins the toolchain when this gate is promoted to a required
# job.
CWIST_WORLDS = cwist-guest cwist-guest-stream
wit-check:
	@if command -v wit-bindgen > /dev/null 2>&1; then \
	    rm -rf .wit-check && mkdir -p .wit-check && \
	    for world in $(CWIST_WORLDS); do \
	        wit-bindgen c wit/ --world $$world --out-dir .wit-check/$$world > /dev/null || exit 1; \
	    done && \
	    rm -rf .wit-check && echo "wit-check: OK"; \
	else \
	    echo "wit-check: wit-bindgen not installed, skipping"; \
	fi

# Spike for the component browser bundle (issue #203, stage 2): transpile the
# wasip2 guest component into JS with jco. Fetches @bytecodealliance/jco via
# npm exec on first use. The output is generated, never committed.
jco-transpile: wasip2-smoke
	npm exec -y --package=@bytecodealliance/jco -- \
	    jco transpile wasip2_smoke.wasm --out-dir .jco-out

# --- Component dispatch guest (issue #203, stage 2) -----------------------------
# Same dispatch boundary as the Emscripten wrapper test, reached through the
# cwist-guest world (wit/cwist.wit) instead of the pointer ABI: wit-bindgen
# generates the canonical ABI shims, wasi-sdk compiles the guest core module
# for wasm32-wasip2, wasm-tools componentizes it, jco transpiles it to JS,
# and a node test drives it through wasm/npm/component.js.
# All artifacts are generated, never committed.
WIT_BINDINGS_DIR = .wit-bindings
COMPONENT_BUILD_DIR = .component-build/$(WASIP2_TARGET)
JCO_GUEST_DIR = .jco-guest/$(WASIP2_TARGET)
# Host shim satisfying the guest's WASI imports: preview2-shim for wasip2
# guests, preview3-shim for wasip3 (0.3) guests.
JCO_SHIM ?= @bytecodealliance/preview2-shim
# Node needs --experimental-wasm-jspi for 0.3 components (JSPI drives the
# canonical-ABI async lowering); 0.2 guests link statically and pass nothing.
NODE_FLAGS ?=

# Bindings are generated per world (each world lowers its own imports; the
# stream world adds the async host import). Generated files keep the world
# name (cwist_guest.h/cwist_guest.c), so each world gets its own directory.
wit-bindings:
	@if command -v wit-bindgen > /dev/null 2>&1; then \
	    rm -rf $(WIT_BINDINGS_DIR) && mkdir -p $(WIT_BINDINGS_DIR) && \
	    for world in $(CWIST_WORLDS); do \
	        wit-bindgen c wit/ --world $$world --out-dir $(WIT_BINDINGS_DIR)/$$world > /dev/null || exit 1; \
	    done && \
	    echo "wit-bindings: OK"; \
	else \
	    echo "wit-bindings: wit-bindgen not installed, skipping"; \
	fi

$(COMPONENT_BUILD_DIR)/guest.o: tests/wasm_component_guest.c wit-bindings
	@mkdir -p $(COMPONENT_BUILD_DIR)
	$(WASI_SDK)/bin/clang --target=$(WASIP2_TARGET) $(WASIP2_CFLAGS) \
	    -I$(WIT_BINDINGS_DIR)/cwist-guest -c -o $@ $<

$(COMPONENT_BUILD_DIR)/cwist_guest.o: wit-bindings
	@mkdir -p $(COMPONENT_BUILD_DIR)
	$(WASI_SDK)/bin/clang --target=$(WASIP2_TARGET) $(WASIP2_CFLAGS) \
	    -I$(WIT_BINDINGS_DIR)/cwist-guest -c -o $@ $(WIT_BINDINGS_DIR)/cwist-guest/cwist_guest.c

# component embed merges the chosen world into wasi-sdk's component-type
# section and emits the final component in one step (wasm-tools >= 1.25).
# The world must be named explicitly once wit/ holds more than one.
$(COMPONENT_BUILD_DIR)/guest.component.wasm: $(COMPONENT_BUILD_DIR)/guest.o \
                                              $(COMPONENT_BUILD_DIR)/cwist_guest.o \
                                              libcwist_wasip2_$(WASIP2_TARGET).a
	$(WASI_SDK)/bin/clang --target=$(WASIP2_TARGET) $(WASIP2_CFLAGS) \
	    -o $(COMPONENT_BUILD_DIR)/guest.core.wasm \
	    $(COMPONENT_BUILD_DIR)/guest.o $(COMPONENT_BUILD_DIR)/cwist_guest.o \
	    $(WIT_BINDINGS_DIR)/cwist-guest/cwist_guest_component_type.o \
	    $(WASIP2_ARCHIVE) $(WASIP2_LDLIBS) \
	    -Wl,--gc-sections -Wl,--allow-undefined -Wl,-z,stack-size=$(WASIP2_STACK_BYTES)
	wasm-tools component embed wit/ --world cwist-guest \
	    $(COMPONENT_BUILD_DIR)/guest.core.wasm -o $@

component-guest: $(COMPONENT_BUILD_DIR)/guest.component.wasm

component-smoke: component-guest
	npm exec -y --package=@bytecodealliance/jco -- \
	    jco transpile $(COMPONENT_BUILD_DIR)/guest.component.wasm --out-dir $(JCO_GUEST_DIR)
	npm install --prefix $(JCO_GUEST_DIR) --no-save --no-fund --no-audit \
	    --silent $(JCO_SHIM)
	JCO_DIR=$(JCO_GUEST_DIR) $(NODE) $(NODE_FLAGS) tests/wasm_component_test.js

# 0.3 variant of the component smoke (issue #203, stage 3 evaluation): same
# guest and assertions, built for wasm32-wasip3, hosted by preview3-shim under
# node's JSPI. Requires wasi-sdk >= 34; the probe fails loudly on older SDKs.
component-smoke-p3:
	@if ! printf 'int main(void){return 0;}\n' | \
	    $(WASI_SDK)/bin/clang --target=wasm32-wasip3 -x c - -o /tmp/.cwist_p3_probe.wasm \
	    > /dev/null 2>&1; then \
	    echo "component-smoke-p3: $(WASI_SDK) has no wasip3 sysroot (needs wasi-sdk >= 34)"; \
	    exit 1; \
	fi; \
	rm -f /tmp/.cwist_p3_probe.wasm
	$(MAKE) component-smoke WASIP2_TARGET=wasm32-wasip3 \
	    JCO_SHIM=@bytecodealliance/preview3-shim NODE_FLAGS=--experimental-wasm-jspi

# --- Streaming dispatch guest (issue #203, stage 3) ---------------------------
# The cwist-guest-stream world adds the async host import send-chunk, which
# 0.2 components cannot lower, so this guest only builds for wasm32-wasip3.
# The node host supplies sendChunk through an esbuild alias, mirroring how a
# bundler consumer wires the import.
STREAM_JCO_DIR = .jco-guest-stream/$(WASIP2_TARGET)

$(COMPONENT_BUILD_DIR)/stream_guest.o: tests/wasm_component_stream_guest.c wit-bindings
	@mkdir -p $(COMPONENT_BUILD_DIR)
	$(WASI_SDK)/bin/clang --target=$(WASIP2_TARGET) $(WASIP2_CFLAGS) \
	    -I$(WIT_BINDINGS_DIR)/cwist-guest-stream -c -o $@ $<

$(COMPONENT_BUILD_DIR)/cwist_guest_stream.o: wit-bindings
	@mkdir -p $(COMPONENT_BUILD_DIR)
	$(WASI_SDK)/bin/clang --target=$(WASIP2_TARGET) $(WASIP2_CFLAGS) \
	    -I$(WIT_BINDINGS_DIR)/cwist-guest-stream -c -o $@ \
	    $(WIT_BINDINGS_DIR)/cwist-guest-stream/cwist_guest_stream.c

$(COMPONENT_BUILD_DIR)/stream_guest.component.wasm: $(COMPONENT_BUILD_DIR)/stream_guest.o \
                                                    $(COMPONENT_BUILD_DIR)/cwist_guest_stream.o \
                                                    libcwist_wasip2_$(WASIP2_TARGET).a
	$(WASI_SDK)/bin/clang --target=$(WASIP2_TARGET) $(WASIP2_CFLAGS) \
	    -o $(COMPONENT_BUILD_DIR)/stream_guest.core.wasm \
	    $(COMPONENT_BUILD_DIR)/stream_guest.o $(COMPONENT_BUILD_DIR)/cwist_guest_stream.o \
	    $(WIT_BINDINGS_DIR)/cwist-guest-stream/cwist_guest_stream_component_type.o \
	    $(WASIP2_ARCHIVE) $(WASIP2_LDLIBS) \
	    -Wl,--gc-sections -Wl,--allow-undefined -Wl,-z,stack-size=$(WASIP2_STACK_BYTES)
	wasm-tools component embed wit/ --world cwist-guest-stream \
	    $(COMPONENT_BUILD_DIR)/stream_guest.core.wasm -o $@

component-stream-smoke: $(COMPONENT_BUILD_DIR)/stream_guest.component.wasm
	npm exec -y --package=@bytecodealliance/jco -- \
	    jco transpile $(COMPONENT_BUILD_DIR)/stream_guest.component.wasm --out-dir $(STREAM_JCO_DIR)
	npm install --prefix $(STREAM_JCO_DIR) --no-save --no-fund --no-audit \
	    --silent @bytecodealliance/preview3-shim
	npm exec -y --package=esbuild -- \
	    esbuild $(STREAM_JCO_DIR)/stream_guest.component.js \
	    --bundle --platform=node --format=esm \
	    --alias:c4punks:cwist/host=$(abspath tests/wasm_component_stream_host.js) \
	    --external:@bytecodealliance/jco-node-fs* \
	    --outfile=$(STREAM_JCO_DIR)/bundle.mjs
	BUNDLE=$(abspath $(STREAM_JCO_DIR)/bundle.mjs) \
	    $(NODE) --experimental-wasm-jspi tests/wasm_component_stream_test.js

component-stream-smoke-p3:
	@if ! printf 'int main(void){return 0;}\n' | \
	    $(WASI_SDK)/bin/clang --target=wasm32-wasip3 -x c - -o /tmp/.cwist_p3_probe.wasm \
	    > /dev/null 2>&1; then \
	    echo "component-stream-smoke-p3: $(WASI_SDK) has no wasip3 sysroot (needs wasi-sdk >= 34)"; \
	    exit 1; \
	fi; \
	rm -f /tmp/.cwist_p3_probe.wasm
	$(MAKE) component-stream-smoke WASIP2_TARGET=wasm32-wasip3

clean-component:
	rm -rf $(WIT_BINDINGS_DIR) .component-build .jco-guest .jco-guest-stream

# Packaging gate for the browser bundle (issue #203, stage 2 remainder): the
# transpiled component plus the cwist-wasm adapter must bundle for a browser
# with no node-only imports. esbuild resolves the shim's browser exports via
# --conditions=browser; --no-nodejs-compat makes jco emit pure browser-style
# wasm loading (fetch), which is exactly what a bundler consumer gets. The
# esbuild exit code is the assertion: the browser bundle cannot execute under
# node (its loader fetches the wasm shards), so no runtime step here.
component-browser-smoke: component-smoke
	npm exec -y --package=@bytecodealliance/jco -- \
	    jco transpile $(COMPONENT_BUILD_DIR)/guest.component.wasm \
	    --out-dir $(JCO_GUEST_DIR)/browser --no-nodejs-compat
	cp tests/wasm_component_browser_entry.js $(JCO_GUEST_DIR)/
	npm exec -y --package=esbuild -- esbuild $(JCO_GUEST_DIR)/wasm_component_browser_entry.js \
	    --bundle --platform=browser --format=esm --conditions=browser \
	    --alias:cwist-guest-component=$(abspath $(JCO_GUEST_DIR)/browser/guest.component.js) \
	    --outfile=$(JCO_GUEST_DIR)/browser_bundle.js
	@echo "component-browser-smoke: PASS (esbuild bundled the guest + adapter for a browser with no node-only imports)" 

# Object Files and Target
OBJS = $(SRCS:.c=.o)
LIB_NAME = libcwist.a
LIBTTAK_DIR = lib/libttak
LIBTTAK_LIB = $(LIBTTAK_DIR)/lib/libttak.a
LIBTTAK_EXTRA_CFLAGS =
ifeq ($(UNAME_S),Darwin)
    LIBTTAK_EXTRA_CFLAGS += -D_DARWIN_C_SOURCE
endif
CJSON_DIR = lib/cjson
CJSON_LIB = $(CJSON_DIR)/libcjson.a
CNATS_DIR = lib/cnats
CNATS_LIB = $(CNATS_DIR)/build/lib/libnats_static.a

# Installation Paths
#
# libcwist.a is deliberately a thin archive: it contains CWIST objects only.
# Third-party static archives are installed side-by-side in DEPSDIR so a
# packaged installation neither duplicates their objects nor hides updates to
# individual dependencies.
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
PCDIR ?= $(LIBDIR)/pkgconfig
DEPSDIR ?= $(LIBDIR)/cwist

# Release version: latest git tag without the leading 'v'.
VERSION ?= $(shell git describe --tags --abbrev=0 2>/dev/null | sed 's/^v//')
ifeq ($(strip $(VERSION)),)
VERSION = 0.0.0
endif

PC_FILE = cwist.pc
VENDOR_INCLUDEDIR ?= $(INCLUDEDIR)/cwist/vendor

INSTALL_LIBDIR = $(DESTDIR)$(LIBDIR)
INSTALL_INCLUDEDIR = $(DESTDIR)$(INCLUDEDIR)
INSTALL_DEPSDIR = $(DESTDIR)$(DEPSDIR)
INSTALL_VENDOR_INCLUDEDIR = $(DESTDIR)$(VENDOR_INCLUDEDIR)
INSTALL_PCDIR = $(DESTDIR)$(PCDIR)

EXTERNAL_LIBS = $(URIPARSER_LIB) \
                $(CJSON_LIB) \
                $(LIBTTAK_LIB) \
                $(CNATS_LIB) \
                $(LSQUIC_LIB) \
                $(BORINGSSL_SSL_LIB) \
                $(BORINGSSL_CRYPTO_LIB)

# --- Build Targets ---

all: $(LIBTTAK_LIB) $(CJSON_LIB) $(URIPARSER_LIB) $(SQLITE_DIR)/sqlite3.c $(LSQUIC_LIB) $(LIB_NAME)

# SQLite Download & Extraction Rule
$(SQLITE_DIR)/sqlite3.c:
	@echo "Downloading SQLite..."
	@mkdir -p $(SQLITE_DIR)
	@wget -q $(SQLITE_URL) -O $(SQLITE_DIR)/$(SQLITE_ZIP)
	@echo "Extracting SQLite..."
	@unzip -q -o -j $(SQLITE_DIR)/$(SQLITE_ZIP) -d $(SQLITE_DIR)
	@rm $(SQLITE_DIR)/$(SQLITE_ZIP)
	@echo "SQLite Ready."

# Ensure lsquic submodule is checked out before compiling objects that need its headers
$(OBJS): | lib/lsquic/include/lsquic.h

lib/lsquic/include/lsquic.h:
	@if [ ! -f "$@" ]; then \
		echo "Initializing lsquic submodule..."; \
		git submodule update --init --recursive $(LSQUIC_DIR); \
	fi

$(LIB_NAME): $(EXTERNAL_LIBS) $(OBJS)
	@echo "Creating thin static library..."
	@rm -f $@
	ar rcs $@ $(OBJS)

$(LIBTTAK_LIB):
	@echo "Building libttak..."
	$(MAKE) -C $(LIBTTAK_DIR) EXTRA_CFLAGS="$(LIBTTAK_EXTRA_CFLAGS)"

$(CJSON_LIB):
	@echo "Building cJSON..."
	$(CC) -O3 -fPIC -I$(CJSON_DIR) -c $(CJSON_DIR)/cJSON.c -o $(CJSON_DIR)/cJSON.o
	ar rcs $@ $(CJSON_DIR)/cJSON.o
	@echo "cJSON Ready."

$(URIPARSER_LIB):
	@echo "Configuring uriparser..."
	cmake -S $(URIPARSER_DIR) -B $(URIPARSER_BUILD_DIR) -DCMAKE_C_COMPILER=$(CC) $(URIPARSER_CMAKE_FLAGS)
	@echo "Building uriparser..."
	cmake --build $(URIPARSER_BUILD_DIR) --target uriparser

BORINGSSL_STAMP = $(BORINGSSL_BUILD_DIR)/.boringssl_built

$(BORINGSSL_STAMP):
	@echo "Building BoringSSL..."
	@mkdir -p $(BORINGSSL_BUILD_DIR)
	cmake -S $(BORINGSSL_DIR) -B $(BORINGSSL_BUILD_DIR) \
		-DCMAKE_C_COMPILER=$(CC) \
		-DCMAKE_CXX_COMPILER=$(CXX) \
		-DCMAKE_BUILD_TYPE=Release
	cmake --build $(BORINGSSL_BUILD_DIR) --target ssl crypto
	@touch $@

$(BORINGSSL_SSL_LIB) $(BORINGSSL_CRYPTO_LIB): $(BORINGSSL_STAMP)

$(LSQUIC_LIB): $(BORINGSSL_SSL_LIB) $(BORINGSSL_CRYPTO_LIB)
	@echo "Building lsquic..."
	@mkdir -p $(LSQUIC_BUILD_DIR)
	cmake -S $(LSQUIC_DIR) -B $(LSQUIC_BUILD_DIR) \
		-DCMAKE_C_COMPILER=$(CC) \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_C_FLAGS="-Wno-unused-function" \
		-DBORINGSSL_DIR=$(abspath $(BORINGSSL_DIR)) \
		-DBORINGSSL_LIB_ssl=$(abspath $(BORINGSSL_SSL_LIB)) \
		-DBORINGSSL_LIB_crypto=$(abspath $(BORINGSSL_CRYPTO_LIB)) \
		-DBORINGSSL_INCLUDE=$(abspath $(BORINGSSL_DIR)/include) \
		-DLSQUIC_WEBTRANSPORT=ON \
		-DBUILD_SHARED_LIBS=OFF
	cmake --build $(LSQUIC_BUILD_DIR) --target lsquic

$(CNATS_LIB):
	@echo "Building cnats..."
	@mkdir -p $(CNATS_DIR)/build
	cmake -S $(CNATS_DIR) -B $(CNATS_DIR)/build \
		-DCMAKE_C_COMPILER=$(CC) \
		-DCMAKE_BUILD_TYPE=Release \
		-DBUILD_SHARED_LIBS=OFF \
		-DNATS_BUILD_WITH_TLS=OFF \
		-DNATS_BUILD_STREAMING=OFF
	cmake --build $(CNATS_DIR)/build --target nats_static

# --- Test Targets ---

TEST_TARGETS = test_worker_affinity \
               test_app_resource_limits \
               test_reactor_wake \
               test_classic_pool_scaling \
               test_reactor_drain_chunk \
               test_latency_probe \
               test_sstring \
               test_seq \
               test_seq_auth \
               test_sha256 \
               test_error \
               test_arena \
               test_wasm_stream \
               test_stream_producer \
               test_healthz \
               test_json_builder \
               test_flash \
               test_html_component \
               test_html_response \
               test_assets \
               test_html_parity \
               test_http \
               test_http_stringify \
               test_siphash \
               test_mux \
               test_mux_param \
               test_rdbms_auto_mount \
               stress_test \
               test_cors \
               test_websocket \
               test_websocket_async \
               test_jwt \
               test_migrate \
               test_json_heal \
               test_https \
               test_http2 \
               test_http2_prebuffer \
               test_http3 \
               test_shutdown \
               test_compress \
               test_log \
               nuke_missing_user_test \
               test_bind \
               test_metrics \
               test_access_log \
               test_rate_limit \
               test_cache \
               test_bdr \
               test_secure_headers \
               test_profile \
               test_http_chunked \
               test_static_and_range \
               test_session \
               test_csrf \
               test_cookie \
               test_waf \
               test_db_pool \
               test_db_memory \
               test_redis \
               test_scheduler \
               test_gc_job_handoff \
               test_async_defer \
               test_async_file_park \
               test_http_fairness \
               test_http_pipeline \
               test_test_client \
               test_multiport \
               test_grpc \
               test_grpc_append_error \
               test_grpc_stream \
               test_grpc_client \
               test_grpc_channel \
               test_dispatch_memory \
               test_gc_ebr_release \
               test_full_gc_toggle_hardening \
               test_conn_registry \
               test_full_gc_sweep \
               test_io_queue_full_gc \
               test_full_gc_ownership_handoff \
               test_defer_free \
               test_malloc_intercept \
               test_proto_gen \
               test_proto_desc \
               test_html_builder \
               test_css_composer \
               test_multipart

.PHONY: all test $(TEST_TARGETS) fuzz_seq install uninstall dist clean rebuild examples clean-examples wasm wasm-smoke clean-wasm wasip2-smoke clean-wasip2 wit-check jco-transpile wit-bindings component-guest component-smoke clean-component

.PHONY: all test $(TEST_TARGETS) fuzz_seq install uninstall dist clean rebuild examples clean-examples wasm wasm-smoke clean-wasm wasip2-smoke clean-wasip2 wit-check jco-transpile wit-bindings component-guest component-smoke clean-component

# Run with e.g. `make fuzz_seq FUZZ_RUNS=100000`.  The target intentionally
# uses a dedicated clang/libFuzzer toolchain and is not part of `make test`.
FUZZ_RUNS ?= 10000
fuzz_seq: $(LIBTTAK_LIB) $(CJSON_LIB) tests/fuzz_seq.c src/core/seq/seq.c src/core/seq/seq_auth.c src/core/mem/alloc.c
	$(FUZZ_CC) $(INCLUDE_PATHS) $(COMMON_DEFINES) -std=c17 -g -O1 \
		-fsanitize=fuzzer,address,undefined -o $@ tests/fuzz_seq.c \
		src/core/seq/seq.c src/core/seq/seq_auth.c src/core/mem/alloc.c \
		$(LIBTTAK_LIB) $(CJSON_LIB) $(BORINGSSL_SSL_LIB) $(BORINGSSL_CRYPTO_LIB) -pthread
	./$@ -runs=$(FUZZ_RUNS)

bench_security_pool: $(LIB_NAME) tests/bench_security_pool.c
	$(CC) $(CFLAGS) -o bench_security_pool tests/bench_security_pool.c $(LIB_NAME) $(LIBS)
	./bench_security_pool

test: $(TEST_TARGETS)

src/sys/app/app.o: src/sys/app/worker_affinity.h

test_worker_affinity: tests/test_worker_affinity.c src/sys/app/worker_affinity.h
	$(CC) $(CFLAGS) -Isrc/sys/app -o $@ tests/test_worker_affinity.c
	./$@

test_app_resource_limits: $(LIB_NAME) tests/test_app_resource_limits.c src/sys/app/worker_affinity.h
	$(CC) $(CFLAGS) -o $@ tests/test_app_resource_limits.c $(LIB_NAME) $(LIBS)
	./$@

# Kernel/queue contract test: use the real reactor with a test-only allocator,
# without pulling HTTP/TLS or libttak runtime state into the wake-up schedule.
test_reactor_wake: tests/test_reactor_wake.c src/sys/io/reactor.c
	$(CC) $(CFLAGS) -o $@ tests/test_reactor_wake.c -pthread
	./$@

test_classic_pool_scaling: $(LIB_NAME) tests/test_classic_pool_scaling.c
	$(CC) $(CFLAGS) -o $@ tests/test_classic_pool_scaling.c $(LIB_NAME) $(LIBS)
	./$@ burst
	./$@ multi
	./$@ cap
	./$@ failure

# Cooperative-queuing correctness test (issue #25): CWIST_REACTOR_DRAIN_CHUNK
# interleaves foreign-thread post draining into a big CQE batch instead of
# only at the batch's end. See tests/bench_cooperative_queuing.c for the
# full before/after latency measurement.
test_reactor_drain_chunk: tests/test_reactor_drain_chunk.c src/sys/io/reactor.c
	$(CC) $(CFLAGS) -o $@ tests/test_reactor_drain_chunk.c -pthread
	./$@

# Latency probe test (issue #166): run twice per build, with the probe
# enabled (recorder must accumulate samples) and disabled (recorder must
# stay at zero). CWIST_LATENCY_PROBE must be set before reactor creation.
test_latency_probe: tests/test_latency_probe.c src/sys/io/reactor.c
	$(CC) $(CFLAGS) -o $@ tests/test_latency_probe.c -pthread
	CWIST_LATENCY_PROBE=1 ./$@
	./$@

bench_cooperative_queuing: tests/bench_cooperative_queuing.c src/sys/io/reactor.c
	$(CC) $(CFLAGS) -o $@ tests/bench_cooperative_queuing.c -pthread

test_sstring: $(LIB_NAME) tests/test_sstring.c
	$(CC) $(CFLAGS) -o test_sstring tests/test_sstring.c $(LIB_NAME) $(LIBS)
	./test_sstring

test_seq: $(LIB_NAME) tests/test_seq.c
	$(CC) $(CFLAGS) -o test_seq tests/test_seq.c $(LIB_NAME) $(LIBS)
	./test_seq

test_seq_auth: $(LIB_NAME) tests/test_seq_auth.c
	$(CC) $(CFLAGS) -o test_seq_auth tests/test_seq_auth.c $(LIB_NAME) $(LIBS)
	./test_seq_auth

test_sha256: tests/test_sha256.c include/cwist/core/crypto/sha256.h
	$(CC) $(CFLAGS) -o $@ tests/test_sha256.c
	./$@

test_error: $(LIB_NAME) tests/test_error.c
	$(CC) $(CFLAGS) -o test_error tests/test_error.c $(LIB_NAME) $(LIBS)
	./test_error

test_arena: $(LIB_NAME) tests/test_arena.c
	$(CC) $(CFLAGS) -o test_arena tests/test_arena.c $(LIB_NAME) $(LIBS)
	./test_arena

test_healthz: $(LIB_NAME) tests/test_healthz.c
	$(CC) $(CFLAGS) -o test_healthz tests/test_healthz.c $(LIB_NAME) $(LIBS)
	./test_healthz

test_wasm_stream: $(LIB_NAME) tests/test_wasm_stream.c
	$(CC) $(CFLAGS) -o test_wasm_stream tests/test_wasm_stream.c $(LIB_NAME) $(LIBS)
	./test_wasm_stream

test_stream_producer: $(LIB_NAME) tests/test_stream_producer.c
	$(CC) $(CFLAGS) -o test_stream_producer tests/test_stream_producer.c $(LIB_NAME) $(LIBS)
	./test_stream_producer

test_json_builder: $(LIB_NAME) tests/test_json_builder.c
	$(CC) $(CFLAGS) -o test_json_builder tests/test_json_builder.c $(LIB_NAME) $(LIBS)
	./test_json_builder

test_flash: $(LIB_NAME) tests/test_flash.c
	$(CC) $(CFLAGS) -o test_flash tests/test_flash.c $(LIB_NAME) $(LIBS)
	./test_flash

test_http: $(LIB_NAME) tests/test_http.c
	$(CC) $(CFLAGS) -o test_http tests/test_http.c $(LIB_NAME) $(LIBS)
	./test_http

test_http_stringify: $(LIB_NAME) tests/test_http_stringify.c
	$(CC) $(CFLAGS) -o test_http_stringify tests/test_http_stringify.c $(LIB_NAME) $(LIBS)
	./test_http_stringify

test_siphash: $(LIB_NAME) tests/test_siphash.c
	$(CC) $(CFLAGS) -o test_siphash tests/test_siphash.c $(LIB_NAME) $(LIBS)
	./test_siphash

test_mux: $(LIB_NAME) tests/test_mux.c
	$(CC) $(CFLAGS) -o test_mux tests/test_mux.c $(LIB_NAME) $(LIBS)
	./test_mux

test_mux_param: $(LIB_NAME) tests/test_mux_param.c
	$(CC) $(CFLAGS) -o test_mux_param tests/test_mux_param.c $(LIB_NAME) $(LIBS)
	./test_mux_param

test_jwt: $(LIB_NAME) tests/test_jwt.c
	$(CC) $(CFLAGS) -o test_jwt tests/test_jwt.c $(LIB_NAME) $(LIBS)
	./test_jwt

test_migrate: $(LIB_NAME) tests/test_migrate.c
	$(CC) $(CFLAGS) -o test_migrate tests/test_migrate.c $(LIB_NAME) $(LIBS)
	./test_migrate

test_json_heal: $(LIB_NAME) tests/test_json_heal.c
	$(CC) $(CFLAGS) -o test_json_heal tests/test_json_heal.c $(LIB_NAME) $(LIBS)
	./test_json_heal

test_https: $(LIB_NAME) tests/test_https.c
	$(CC) $(CFLAGS) -o test_https tests/test_https.c $(LIB_NAME) $(LIBS)
	./test_https

test_http2: $(LIB_NAME) tests/test_http2.c
	$(CC) $(CFLAGS) -o test_http2 tests/test_http2.c $(LIB_NAME) $(LIBS)
	./test_http2

test_http2_prebuffer: $(LIB_NAME) tests/test_http2_prebuffer.c
	$(CC) $(CFLAGS) -o test_http2_prebuffer tests/test_http2_prebuffer.c $(LIB_NAME) $(LIBS)
	./test_http2_prebuffer

# Standalone h2c server for external conformance tools (h2spec); build-only,
# executed by the interop CI job, not by `make test`.
h2c-server: $(LIB_NAME) tests/h2c_server.c
	$(CC) $(CFLAGS) -o h2c-server tests/h2c_server.c $(LIB_NAME) $(LIBS)

test_http3: $(LIB_NAME) tests/test_http3.c
	$(CC) $(CFLAGS) -o test_http3 tests/test_http3.c $(LIB_NAME) $(LIBS)
	./test_http3

test_rdbms_auto_mount: $(LIB_NAME) tests/test_rdbms_auto_mount.c
	$(CC) $(CFLAGS) -o test_rdbms_auto_mount tests/test_rdbms_auto_mount.c $(LIB_NAME) $(LIBS)
	./test_rdbms_auto_mount

stress_test: $(LIB_NAME) tests/stress_test.c
	$(CC) $(CFLAGS) -o stress_test tests/stress_test.c $(LIB_NAME) $(LIBS)
	./stress_test

test_cors: $(LIB_NAME) tests/test_cors.c
	$(CC) $(CFLAGS) -o test_cors tests/test_cors.c $(LIB_NAME) $(LIBS)
	./test_cors

test_websocket: $(LIB_NAME) tests/test_websocket.c
	$(CC) $(CFLAGS) -o test_websocket tests/test_websocket.c $(LIB_NAME) $(LIBS)
	./test_websocket

test_websocket_async: $(LIB_NAME) tests/test_websocket_async.c
	$(CC) $(CFLAGS) -o test_websocket_async tests/test_websocket_async.c $(LIB_NAME) $(LIBS)
	./test_websocket_async

test_shutdown: $(LIB_NAME) tests/test_shutdown.c
	$(CC) $(CFLAGS) -o test_shutdown tests/test_shutdown.c $(LIB_NAME) $(LIBS)
	./test_shutdown

test_compress: $(LIB_NAME) tests/test_compress.c
	$(CC) $(CFLAGS) -o test_compress tests/test_compress.c $(LIB_NAME) $(LIBS)
	./test_compress

test_log: $(LIB_NAME) tests/test_log.c
	$(CC) $(CFLAGS) -o test_log tests/test_log.c $(LIB_NAME) $(LIBS)
	./test_log

nuke_missing_user_test: $(LIB_NAME) tests/nuke_missing_user_test.c
	$(CC) $(CFLAGS) -o nuke_missing_user_test tests/nuke_missing_user_test.c $(LIB_NAME) $(LIBS)
	./nuke_missing_user_test

test_bind: $(LIB_NAME) tests/test_bind.c
	$(CC) $(CFLAGS) -o test_bind tests/test_bind.c $(LIB_NAME) $(LIBS)
	./test_bind

test_metrics: $(LIB_NAME) tests/test_metrics.c
	$(CC) $(CFLAGS) -o test_metrics tests/test_metrics.c $(LIB_NAME) $(LIBS)
	./test_metrics

test_access_log: $(LIB_NAME) tests/test_access_log.c
	$(CC) $(CFLAGS) -o test_access_log tests/test_access_log.c $(LIB_NAME) $(LIBS)
	./test_access_log

test_secure_headers: $(LIB_NAME) tests/test_secure_headers.c
	$(CC) $(CFLAGS) -o test_secure_headers tests/test_secure_headers.c $(LIB_NAME) $(LIBS)
	./test_secure_headers

test_profile: $(LIB_NAME) tests/test_profile.c
	$(CC) $(CFLAGS) -o test_profile tests/test_profile.c $(LIB_NAME) $(LIBS)
	./test_profile

test_rate_limit: $(LIB_NAME) tests/test_rate_limit.c
	$(CC) $(CFLAGS) -o test_rate_limit tests/test_rate_limit.c $(LIB_NAME) $(LIBS)
	./test_rate_limit

test_cache: $(LIB_NAME) tests/test_cache.c
	$(CC) $(CFLAGS) -o test_cache tests/test_cache.c $(LIB_NAME) $(LIBS)
	./test_cache

test_bdr: $(LIB_NAME) tests/test_bdr.c
	$(CC) $(CFLAGS) -o test_bdr tests/test_bdr.c $(LIB_NAME) $(LIBS)
	./test_bdr

install: $(LIB_NAME) $(PC_FILE)
	@echo "Installing CWIST library to $(LIBDIR)..."
	install -d $(INSTALL_LIBDIR)
	install -m 644 $(LIB_NAME) $(INSTALL_LIBDIR)/
	@echo "Installing external archives to $(DEPSDIR)..."
	install -d $(INSTALL_DEPSDIR)
	install -m 644 $(EXTERNAL_LIBS) $(INSTALL_DEPSDIR)/
	@echo "Installing CWIST headers to $(INCLUDEDIR)/cwist..."
	install -d $(INSTALL_INCLUDEDIR)/cwist
	cp -R include/cwist/. $(INSTALL_INCLUDEDIR)/cwist/
	find $(INSTALL_INCLUDEDIR)/cwist -type d -exec chmod 755 {} \;
	find $(INSTALL_INCLUDEDIR)/cwist -type f -exec chmod 644 {} \;
	@echo "Installing bundled dependency headers to $(VENDOR_INCLUDEDIR)..."
	install -d $(INSTALL_VENDOR_INCLUDEDIR)/cjson $(INSTALL_VENDOR_INCLUDEDIR)/ttak $(INSTALL_VENDOR_INCLUDEDIR)/openssl $(INSTALL_VENDOR_INCLUDEDIR)/lsquic $(INSTALL_VENDOR_INCLUDEDIR)/uriparser
	install -m 644 $(CJSON_DIR)/cJSON.h $(INSTALL_VENDOR_INCLUDEDIR)/cjson/
	cp -R $(LIBTTAK_DIR)/include/ttak/. $(INSTALL_VENDOR_INCLUDEDIR)/ttak/
	cp -R $(BORINGSSL_DIR)/include/openssl/. $(INSTALL_VENDOR_INCLUDEDIR)/openssl/
	install -m 644 $(LSQUIC_DIR)/include/lsquic.h $(INSTALL_VENDOR_INCLUDEDIR)/lsquic/
	cp -R $(URIPARSER_DIR)/include/uriparser/. $(INSTALL_VENDOR_INCLUDEDIR)/uriparser/
	install -m 644 $(SQLITE_DIR)/sqlite3.h $(SQLITE_DIR)/sqlite3ext.h $(INSTALL_VENDOR_INCLUDEDIR)/
	find $(INSTALL_VENDOR_INCLUDEDIR) -type d -exec chmod 755 {} \;
	find $(INSTALL_VENDOR_INCLUDEDIR) -type f -exec chmod 644 {} \;
	@echo "Installing pkg-config file to $(PCDIR)..."
	install -d $(INSTALL_PCDIR)
	install -m 644 $(PC_FILE) $(INSTALL_PCDIR)/
	@echo "Installing license documentation to $(PREFIX)/share/doc/cwist..."
	install -d $(DESTDIR)$(PREFIX)/share/doc/cwist/licenses
	install -m 644 LICENSE NOTICE.md $(DESTDIR)$(PREFIX)/share/doc/cwist/
	@set -e; for spec in \
		"$(BORINGSSL_DIR)/LICENSE:boringssl:LICENSE" \
		"$(LSQUIC_DIR)/LICENSE:lsquic:LICENSE" \
		"$(LSQUIC_DIR)/LICENSE.chrome:lsquic:LICENSE.chrome" \
		"$(LIBTTAK_DIR)/LICENSE:libttak:LICENSE" \
		"$(CJSON_DIR)/LICENSE:cjson:LICENSE" \
		"lib/cnats/LICENSE:cnats:LICENSE" \
		"$(URIPARSER_DIR)/COPYING.BSD-3-Clause:uriparser:COPYING.BSD-3-Clause"; do \
		src="$${spec%%:*}"; rest="$${spec#*:}"; comp="$${rest%%:*}"; \
		if [ -f "$$src" ]; then \
			install -d $(DESTDIR)$(PREFIX)/share/doc/cwist/licenses/$$comp; \
			install -m 644 "$$src" $(DESTDIR)$(PREFIX)/share/doc/cwist/licenses/$$comp/; \
		fi; \
	done
	@echo "Installing cwist CLI to $(BINDIR)..."
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 tools/cli/cwist $(DESTDIR)$(BINDIR)/cwist
	@echo "Installation complete.  Compile with: pkg-config --cflags --libs cwist"

$(PC_FILE): cwist.pc.in
	sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@VERSION@|$(VERSION)|g' cwist.pc.in > $@

uninstall:
	@echo "Uninstalling cwist..."
	rm -f $(DESTDIR)$(LIBDIR)/$(LIB_NAME)
	rm -f $(DESTDIR)$(DEPSDIR)/liburiparser.a $(DESTDIR)$(DEPSDIR)/libcjson.a $(DESTDIR)$(DEPSDIR)/libttak.a $(DESTDIR)$(DEPSDIR)/libnats_static.a $(DESTDIR)$(DEPSDIR)/liblsquic.a $(DESTDIR)$(DEPSDIR)/libssl.a $(DESTDIR)$(DEPSDIR)/libcrypto.a
	rmdir $(DESTDIR)$(DEPSDIR) 2>/dev/null || true
	rm -rf $(DESTDIR)$(INCLUDEDIR)/cwist
	rm -f $(INSTALL_PCDIR)/$(PC_FILE)
	rm -f $(DESTDIR)$(BINDIR)/cwist
	rm -rf $(DESTDIR)$(PREFIX)/share/doc/cwist
	@echo "Uninstallation complete."

# Source release tarball with all vendored submodule sources, so the
# archive builds on a machine without git submodule access. The SQLite
# amalgamation is still downloaded by the build itself when absent.
DIST_DIR = dist
DIST_NAME = cwist-$(VERSION)

dist:
	@echo "Creating $(DIST_DIR)/$(DIST_NAME).tar.gz (with vendored sources)..."
	@mkdir -p $(DIST_DIR)
	@rm -rf $(DIST_DIR)/$(DIST_NAME)
	@mkdir -p $(DIST_DIR)/$(DIST_NAME)
	git ls-files --recurse-submodules | tar cf - -T - | (cd $(DIST_DIR)/$(DIST_NAME) && tar xf -)
	tar czf $(DIST_DIR)/$(DIST_NAME).tar.gz -C $(DIST_DIR) $(DIST_NAME)
	@rm -rf $(DIST_DIR)/$(DIST_NAME)
	@echo "Done: $(DIST_DIR)/$(DIST_NAME).tar.gz"

clean:
	@echo "Cleaning up build artifacts..."
	rm -f $(OBJS) $(LIB_NAME) $(PC_FILE)
	rm -rf include/cwist/vendor
	rm -f $(TEST_TARGETS)
	rm -f $(CJSON_DIR)/cJSON.o $(CJSON_LIB)
	rm -rf $(URIPARSER_BUILD_DIR)
	@rm -rf $(CNATS_DIR)/build
	@rm -rf $(BORINGSSL_BUILD_DIR)
	@rm -rf $(LSQUIC_BUILD_DIR)
	-@$(MAKE) -C $(LIBTTAK_DIR) clean || true

rebuild: clean all

# ------------------------------------------------------------------
# Examples
# ------------------------------------------------------------------

EXAMPLE_BINS = example/simple-server/simple-server \
               example/http/step-1-hello-world/hello-world \
               example/http/step-2-mux-router/mux-router \
               example/http/step-3-query-params/query-params \
               example/http/step-4-json-api/json-api \
               example/jwt/step-2-http-auth/jwt-auth \
               example/cde-json-viewer/cde-json-viewer \
               example/db/step-1-open-query/open-query \
               example/db/step-2-migrations/migrations \
               example/db/step-4-json-insert/json-insert \
               example/rps-showcase/rps-showcase

examples: $(EXAMPLE_BINS)

example/simple-server/simple-server: $(LIB_NAME) example/simple-server/main.c
	$(CC) $(CFLAGS) -o $@ example/simple-server/main.c $(LIB_NAME) $(LIBS)

example/http/step-1-hello-world/hello-world: $(LIB_NAME) example/http/step-1-hello-world/main.c
	$(CC) $(CFLAGS) -o $@ example/http/step-1-hello-world/main.c $(LIB_NAME) $(LIBS)

example/http/step-2-mux-router/mux-router: $(LIB_NAME) example/http/step-2-mux-router/main.c
	$(CC) $(CFLAGS) -o $@ example/http/step-2-mux-router/main.c $(LIB_NAME) $(LIBS)

example/http/step-3-query-params/query-params: $(LIB_NAME) example/http/step-3-query-params/main.c
	$(CC) $(CFLAGS) -o $@ example/http/step-3-query-params/main.c $(LIB_NAME) $(LIBS)

example/http/step-4-json-api/json-api: $(LIB_NAME) example/http/step-4-json-api/main.c
	$(CC) $(CFLAGS) -o $@ example/http/step-4-json-api/main.c $(LIB_NAME) $(LIBS)

example/jwt/step-2-http-auth/jwt-auth: $(LIB_NAME) example/jwt/step-2-http-auth/main.c
	$(CC) $(CFLAGS) -o $@ example/jwt/step-2-http-auth/main.c $(LIB_NAME) $(LIBS)

example/cde-json-viewer/cde-json-viewer: $(LIB_NAME) example/cde-json-viewer/main.c
	$(CC) $(CFLAGS) -o $@ example/cde-json-viewer/main.c $(LIB_NAME) $(LIBS)

example/db/step-1-open-query/open-query: $(LIB_NAME) example/db/step-1-open-query/main.c
	$(CC) $(CFLAGS) -o $@ example/db/step-1-open-query/main.c $(LIB_NAME) $(LIBS)

example/db/step-2-migrations/migrations: $(LIB_NAME) example/db/step-2-migrations/main.c
	$(CC) $(CFLAGS) -o $@ example/db/step-2-migrations/main.c $(LIB_NAME) $(LIBS)

example/db/step-4-json-insert/json-insert: $(LIB_NAME) example/db/step-4-json-insert/main.c
	$(CC) $(CFLAGS) -o $@ example/db/step-4-json-insert/main.c $(LIB_NAME) $(LIBS)

example/rps-showcase/rps-showcase: $(LIB_NAME) example/rps-showcase/main.c
	$(CC) $(CFLAGS) -o $@ example/rps-showcase/main.c $(LIB_NAME) $(LIBS)

# Micro examples
MICRO_BINS = example/micro/01-hello/hello \
             example/micro/02-routes/routes \
             example/micro/03-path-params/path-params \
             example/micro/04-query-params/query-params \
             example/micro/05-json/json \
             example/micro/06-orm-insert/orm-insert \
             example/micro/07-orm-query/orm-query \
             example/micro/08-orm-update-delete/orm-update-delete \
             example/micro/09-html-builder/html-builder \
             example/micro/10-static-files/static-files \
             example/micro/11-jwt-auth/jwt-auth \
             example/micro/12-middleware/middleware \
             example/micro/13-nuke-db/nuke-db \
             example/micro/14-websocket/websocket \
             example/micro/15-pqc-tls/pqc-tls \
             example/micro/16-rdbms-auto/rdbms-auto \
             example/micro/17-blog-crud/blog-crud

micro-examples: $(MICRO_BINS)

example/micro/01-hello/hello: $(LIB_NAME) example/micro/01-hello/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/01-hello/main.c $(LIB_NAME) $(LIBS)

example/micro/02-routes/routes: $(LIB_NAME) example/micro/02-routes/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/02-routes/main.c $(LIB_NAME) $(LIBS)

example/micro/03-path-params/path-params: $(LIB_NAME) example/micro/03-path-params/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/03-path-params/main.c $(LIB_NAME) $(LIBS)

example/micro/04-query-params/query-params: $(LIB_NAME) example/micro/04-query-params/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/04-query-params/main.c $(LIB_NAME) $(LIBS)

example/micro/05-json/json: $(LIB_NAME) example/micro/05-json/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/05-json/main.c $(LIB_NAME) $(LIBS)

example/micro/06-orm-insert/orm-insert: $(LIB_NAME) example/micro/06-orm-insert/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/06-orm-insert/main.c $(LIB_NAME) $(LIBS)

example/micro/07-orm-query/orm-query: $(LIB_NAME) example/micro/07-orm-query/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/07-orm-query/main.c $(LIB_NAME) $(LIBS)

example/micro/08-orm-update-delete/orm-update-delete: $(LIB_NAME) example/micro/08-orm-update-delete/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/08-orm-update-delete/main.c $(LIB_NAME) $(LIBS)

example/micro/09-html-builder/html-builder: $(LIB_NAME) example/micro/09-html-builder/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/09-html-builder/main.c $(LIB_NAME) $(LIBS)

example/micro/10-static-files/static-files: $(LIB_NAME) example/micro/10-static-files/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/10-static-files/main.c $(LIB_NAME) $(LIBS)

example/micro/11-jwt-auth/jwt-auth: $(LIB_NAME) example/micro/11-jwt-auth/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/11-jwt-auth/main.c $(LIB_NAME) $(LIBS)

example/micro/12-middleware/middleware: $(LIB_NAME) example/micro/12-middleware/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/12-middleware/main.c $(LIB_NAME) $(LIBS)

example/micro/13-nuke-db/nuke-db: $(LIB_NAME) example/micro/13-nuke-db/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/13-nuke-db/main.c $(LIB_NAME) $(LIBS)

example/micro/14-websocket/websocket: $(LIB_NAME) example/micro/14-websocket/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/14-websocket/main.c $(LIB_NAME) $(LIBS)

example/micro/15-pqc-tls/pqc-tls: $(LIB_NAME) example/micro/15-pqc-tls/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/15-pqc-tls/main.c $(LIB_NAME) $(LIBS)

example/micro/16-rdbms-auto/rdbms-auto: $(LIB_NAME) example/micro/16-rdbms-auto/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/16-rdbms-auto/main.c $(LIB_NAME) $(LIBS)

example/micro/17-blog-crud/blog-crud: $(LIB_NAME) example/micro/17-blog-crud/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/17-blog-crud/main.c $(LIB_NAME) $(LIBS)

clean-examples:
	rm -f $(EXAMPLE_BINS) $(MICRO_BINS)

test_http_chunked: $(LIB_NAME) tests/test_http_chunked.c
	$(CC) $(CFLAGS) -o test_http_chunked tests/test_http_chunked.c $(LIB_NAME) $(LIBS)
	./test_http_chunked

test_static_and_range: $(LIB_NAME) tests/test_static_and_range.c
	$(CC) $(CFLAGS) -o test_static_and_range tests/test_static_and_range.c $(LIB_NAME) $(LIBS)
	./test_static_and_range

test_session: $(LIB_NAME) tests/test_session.c
	$(CC) $(CFLAGS) -o test_session tests/test_session.c $(LIB_NAME) $(LIBS)
	./test_session

test_csrf: $(LIB_NAME) tests/test_csrf.c
	$(CC) $(CFLAGS) -o test_csrf tests/test_csrf.c $(LIB_NAME) $(LIBS)
	./test_csrf

test_cookie: $(LIB_NAME) tests/test_cookie.c
	$(CC) $(CFLAGS) -o test_cookie tests/test_cookie.c $(LIB_NAME) $(LIBS)
	./test_cookie

test_multipart: $(LIB_NAME) tests/test_multipart.c
	$(CC) $(CFLAGS) -o test_multipart tests/test_multipart.c $(LIB_NAME) $(LIBS)
	./test_multipart

test_waf: $(LIB_NAME) tests/test_waf.c
	$(CC) $(CFLAGS) -o test_waf tests/test_waf.c $(LIB_NAME) $(LIBS)
	./test_waf

test_db_pool: $(LIB_NAME) tests/test_db_pool.c
	$(CC) $(CFLAGS) -o test_db_pool tests/test_db_pool.c $(LIB_NAME) $(LIBS)
	./test_db_pool

test_db_memory: $(LIB_NAME) tests/test_db_memory.c
	$(CC) $(CFLAGS) -o test_db_memory tests/test_db_memory.c $(LIB_NAME) $(LIBS)
	./test_db_memory

test_redis: $(LIB_NAME) tests/test_redis.c
	$(CC) $(CFLAGS) -o test_redis tests/test_redis.c $(LIB_NAME) $(LIBS)
	./test_redis

test_sse: $(LIB_NAME) tests/test_sse.c
	$(CC) $(CFLAGS) -o test_sse tests/test_sse.c $(LIB_NAME) $(LIBS)
	./test_sse

test_graphql: $(LIB_NAME) tests/test_graphql.c
	$(CC) $(CFLAGS) -o test_graphql tests/test_graphql.c $(LIB_NAME) $(LIBS)
	./test_graphql

test_core_hardening: $(LIB_NAME) tests/test_core_hardening.c
	$(CC) $(CFLAGS) -o test_core_hardening tests/test_core_hardening.c $(LIB_NAME) $(LIBS)
	./test_core_hardening

cli:
	chmod +x tools/cli/cwist
	@echo "CLI ready: ./tools/cli/cwist"

test_scheduler: $(LIB_NAME) tests/test_scheduler.c
	$(CC) $(CFLAGS) -o test_scheduler tests/test_scheduler.c $(LIB_NAME) $(LIBS)
	./test_scheduler

test_gc_job_handoff: $(LIB_NAME) tests/test_gc_job_handoff.c
	$(CC) $(CFLAGS) -o $@ tests/test_gc_job_handoff.c $(LIB_NAME) $(LIBS)
	./$@
	./$@ nogc
	$(CC) $(CFLAGS) -DNDEBUG -o $@ tests/test_gc_job_handoff.c $(LIB_NAME) $(LIBS)
	./$@
	./$@ nogc

test_http_pipeline: $(LIB_NAME) tests/test_http_pipeline.c
	$(CC) $(CFLAGS) -o $@ tests/test_http_pipeline.c $(LIB_NAME) $(LIBS)
	./$@

test_http_fairness: $(LIB_NAME) tests/test_http_fairness.c
	$(CC) $(CFLAGS) -o $@ tests/test_http_fairness.c $(LIB_NAME) $(LIBS)
	./$@
	CWIST_TEST_HALF_CLOSE=1 ./$@
	CWIST_TEST_HALF_CLOSE=1 CWIST_TEST_TRUNCATED=1 ./$@
	CWIST_TEST_DESTROY_PENDING=1 ./$@

test_async_defer: $(LIB_NAME) tests/test_async_defer.c
	$(CC) $(CFLAGS) -o test_async_defer tests/test_async_defer.c $(LIB_NAME) $(LIBS)
	./test_async_defer
	CWIST_C1M_MODE=0 ./test_async_defer

test_async_file_park: $(LIB_NAME) tests/test_async_file_park.c
	$(CC) $(CFLAGS) -o test_async_file_park tests/test_async_file_park.c $(LIB_NAME) $(LIBS)
	./test_async_file_park

test_test_client: $(LIB_NAME) tests/test_test_client.c
	$(CC) $(CFLAGS) -o test_test_client tests/test_test_client.c $(LIB_NAME) $(LIBS)
	./test_test_client

test_multiport: $(LIB_NAME) tests/test_multiport.c
	$(CC) $(CFLAGS) -o test_multiport tests/test_multiport.c $(LIB_NAME) $(LIBS)
	./test_multiport

test_grpc: $(LIB_NAME) tests/test_grpc.c
	$(CC) $(CFLAGS) -o test_grpc tests/test_grpc.c $(LIB_NAME) $(LIBS)
	./test_grpc

test_grpc_append_error: $(LIB_NAME) tests/test_grpc_append_error.c
	$(CC) $(CFLAGS) -o test_grpc_append_error tests/test_grpc_append_error.c $(LIB_NAME) $(LIBS)
	./test_grpc_append_error

test_grpc_stream: $(LIB_NAME) tests/test_grpc_stream.c
	$(CC) $(CFLAGS) -o test_grpc_stream tests/test_grpc_stream.c $(LIB_NAME) $(LIBS)
	./test_grpc_stream

test_grpc_client: $(LIB_NAME) tests/test_grpc_client.c
	$(CC) $(CFLAGS) -o test_grpc_client tests/test_grpc_client.c $(LIB_NAME) $(LIBS)
	./test_grpc_client

test_grpc_channel: $(LIB_NAME) tests/test_grpc_channel.c
	$(CC) $(CFLAGS) -o test_grpc_channel tests/test_grpc_channel.c $(LIB_NAME) $(LIBS)
	./test_grpc_channel

test_dispatch_memory: $(LIB_NAME) tests/test_dispatch_memory.c
	$(CC) $(CFLAGS) -o test_dispatch_memory tests/test_dispatch_memory.c $(LIB_NAME) $(LIBS)
	./test_dispatch_memory

test_gc_ebr_release: $(LIB_NAME) tests/test_gc_ebr_release.c
	$(CC) $(CFLAGS) -o test_gc_ebr_release tests/test_gc_ebr_release.c $(LIB_NAME) $(LIBS)
	./test_gc_ebr_release

test_full_gc_toggle_hardening: $(LIB_NAME) tests/test_full_gc_toggle_hardening.c
	$(CC) $(CFLAGS) -o test_full_gc_toggle_hardening tests/test_full_gc_toggle_hardening.c $(LIB_NAME) $(LIBS)
	./test_full_gc_toggle_hardening

test_conn_registry: $(LIB_NAME) tests/test_conn_registry.c
	$(CC) $(CFLAGS) -o test_conn_registry tests/test_conn_registry.c $(LIB_NAME) $(LIBS)
	./test_conn_registry

test_https_full_gc: $(LIB_NAME) tests/test_https_full_gc.c
	$(CC) $(CFLAGS) -o test_https_full_gc tests/test_https_full_gc.c $(LIB_NAME) $(LIBS)
	./test_https_full_gc

test_full_gc_sweep: $(LIB_NAME) tests/test_full_gc_sweep.c
	$(CC) $(CFLAGS) -o test_full_gc_sweep tests/test_full_gc_sweep.c $(LIB_NAME) $(LIBS)
	./test_full_gc_sweep

test_io_queue_full_gc: $(LIB_NAME) tests/test_io_queue_full_gc.c
	$(CC) $(CFLAGS) -o test_io_queue_full_gc tests/test_io_queue_full_gc.c $(LIB_NAME) $(LIBS)
	./test_io_queue_full_gc

test_full_gc_ownership_handoff: $(LIB_NAME) tests/test_full_gc_ownership_handoff.c
	$(CC) $(CFLAGS) -o test_full_gc_ownership_handoff tests/test_full_gc_ownership_handoff.c $(LIB_NAME) $(LIBS)
	./test_full_gc_ownership_handoff

test_defer_free: $(LIB_NAME) tests/test_defer_free.c
	$(CC) $(CFLAGS) -o test_defer_free tests/test_defer_free.c $(LIB_NAME) $(LIBS)
	./test_defer_free

test_malloc_intercept: $(LIB_NAME) tests/test_malloc_intercept.c
	$(CC) $(CFLAGS) -o test_malloc_intercept tests/test_malloc_intercept.c $(LIB_NAME) $(LIBS)
	./test_malloc_intercept

# Manual throughput probe (not part of `make test`): quantifies the cost of
# CWIST_INTERCEPT_MALLOC's fallback path against a plain, unshimmed
# malloc/free baseline, with full-GC both off and on. See the file's
# header comment for how to read the numbers.
bench_malloc_intercept: $(LIB_NAME) tests/bench_malloc_intercept.c
	$(CC) $(CFLAGS) -o bench_malloc_intercept tests/bench_malloc_intercept.c $(LIB_NAME) $(LIBS)

bench_malloc_intercept_baseline: tests/bench_malloc_intercept.c
	$(CC) $(CFLAGS) -DBASELINE -o bench_malloc_intercept_baseline tests/bench_malloc_intercept.c

# Full-GC pending-set cost vs live-set size and thread count (issue #65).
# Manual probe, not part of `make test`: ./bench_full_gc_tracking on|off [iterations]
bench_full_gc_tracking: $(LIB_NAME) tests/bench_full_gc_tracking.c
	$(CC) $(CFLAGS) -o bench_full_gc_tracking tests/bench_full_gc_tracking.c $(LIB_NAME) $(LIBS)

test_proto_gen: $(LIB_NAME) tests/test_proto_gen.c tests/test_proto_gen_sample.proto
	./tools/cli/cwist proto tests/test_proto_gen_sample.proto --output tests/test_proto_gen_sample.cwist.pb.h
	$(CC) $(CFLAGS) -Itests -o test_proto_gen tests/test_proto_gen.c $(LIB_NAME) $(LIBS)
	./test_proto_gen

# Descriptor-set input: hand-encode a FileDescriptorSet (no protoc needed in
# CI), generate from it, and prove byte-identical output against the text path
# plus a full encode/decode round trip of the generated code.
test_proto_desc: $(LIB_NAME) tests/test_proto_gen.c tests/make_sample_descriptor.py
	python3 tests/make_sample_descriptor.py tests/test_proto_gen_sample.pb
	./tools/cli/cwist proto tests/test_proto_gen_sample.pb --output tests/test_proto_gen_desc_sample.cwist.pb.h
	./tools/cli/cwist proto tests/test_proto_gen_sample.proto --output tests/test_proto_gen_sample.cwist.pb.h
	diff tests/test_proto_gen_sample.cwist.pb.h tests/test_proto_gen_desc_sample.cwist.pb.h
	$(CC) $(CFLAGS) -Itests -DPROTO_GEN_SAMPLE_HEADER='"test_proto_gen_desc_sample.cwist.pb.h"' -o test_proto_desc tests/test_proto_gen.c $(LIB_NAME) $(LIBS)
	./test_proto_desc

test_html_builder: $(LIB_NAME) tests/test_html_builder.c
	$(CC) $(CFLAGS) -o test_html_builder tests/test_html_builder.c $(LIB_NAME) $(LIBS)
	./test_html_builder

test_css_composer: $(LIB_NAME) tests/test_css_composer.c
	$(CC) $(CFLAGS) -o test_css_composer tests/test_css_composer.c $(LIB_NAME) $(LIBS)
	./test_css_composer
test_html_component: $(LIB_NAME) tests/test_html_component.c
	$(CC) $(CFLAGS) -o test_html_component tests/test_html_component.c $(LIB_NAME) $(LIBS)
	./test_html_component

test_html_response: $(LIB_NAME) tests/test_html_response.c
	$(CC) $(CFLAGS) -o test_html_response tests/test_html_response.c $(LIB_NAME) $(LIBS)
	./test_html_response

test_assets: $(LIB_NAME) tests/test_assets.c
	$(CC) $(CFLAGS) -o test_assets tests/test_assets.c $(LIB_NAME) $(LIBS)
	./test_assets

test_html_parity: $(LIB_NAME) tests/test_html_parity.c tests/html_views_shared.h
	$(CC) $(CFLAGS) -o test_html_parity tests/test_html_parity.c $(LIB_NAME) $(LIBS)
	./test_html_parity

test_http2_flow_control: $(LIB_NAME) tests/test_http2_flow_control.c
	$(CC) $(CFLAGS) -o test_http2_flow_control tests/test_http2_flow_control.c $(LIB_NAME) $(LIBS)
	./test_http2_flow_control

test_idle_reaper: $(LIB_NAME) tests/test_idle_reaper.c
	$(CC) $(CFLAGS) -o test_idle_reaper tests/test_idle_reaper.c $(LIB_NAME) $(LIBS)
	./test_idle_reaper

test_linux_writer_fast: $(LIB_NAME) tests/test_linux_writer_fast.c
	$(CC) $(CFLAGS) -o test_linux_writer_fast tests/test_linux_writer_fast.c $(LIB_NAME) $(LIBS)
	./test_linux_writer_fast

test_orm_socket: $(LIB_NAME) tests/test_orm_socket.c
	$(CC) $(CFLAGS) -o test_orm_socket tests/test_orm_socket.c $(LIB_NAME) $(LIBS)
	./test_orm_socket




# ---------------------------------------------------------------------------
# Source formatting (clang-format; rules and their rationale in .clang-format)
#
# Scoped to the trees CWIST owns.  lib/ is vendored and carries its own
# .clang-format with DisableFormat, so it stays untouched either way.
#
# Tracked files only: `make test` drops generated headers (tests/*.cwist.pb.h)
# into these trees, and a plain find would format build output and then fail
# format-check on it.
# ---------------------------------------------------------------------------
FORMAT_DIRS := src include tests example benchmarks
FORMAT_FILES := $(shell git ls-files $(FORMAT_DIRS) 2>/dev/null | grep -E '\.(c|h)$$' \
                  || find $(FORMAT_DIRS) -type f \( -name '*.c' -o -name '*.h' \) | sort)

.PHONY: format format-check

format:
	@clang-format -i $(FORMAT_FILES)
	@echo "clang-format: reformatted $(words $(FORMAT_FILES)) files in $(FORMAT_DIRS)"

format-check:
	@clang-format --dry-run --Werror $(FORMAT_FILES)
	@echo "clang-format: $(words $(FORMAT_FILES)) files already conform"
