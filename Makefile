# Compiler and Flags
CC ?= gcc
FUZZ_CC ?= clang

CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
NGHTTP2_CFLAGS := $(shell pkg-config --cflags libnghttp2 2>/dev/null)
BROTLI_CFLAGS := $(shell pkg-config --cflags libbrotlienc libbrotlicommon libbrotlidec 2>/dev/null)

INCLUDE_PATHS = -I./include -I./lib -I./lib/libttak/include -I./lib/cjson -I./lib/sqlite3 -I./lib/uriparser/include -I./lib/cnats/src -I./lib/boringssl/include -I./lib/lsquic/include -I./lib/multipart-parser-c $(CURL_CFLAGS) $(NGHTTP2_CFLAGS) $(BROTLI_CFLAGS)
COMMON_DEFINES = -D_GNU_SOURCE -D_XOPEN_SOURCE=700 -D_REENTRANT -DSQLITE_ENABLE_DESERIALIZE
COMMON_WARNINGS = -std=c2x -Wall -pthread -fPIC
COMMON_CFLAGS = $(INCLUDE_PATHS) $(COMMON_WARNINGS) $(COMMON_DEFINES)

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
GCC_STACK_FLAGS = -Ofast -g \
                  -flto=auto -ffat-lto-objects \
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
else
    CFLAGS = $(COMMON_CFLAGS) $(PERF_WARNINGS) $(PERF_STACK_FLAGS)
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
SQLITE_YEAR = 2024
SQLITE_VER = 3450100
SQLITE_ZIP = sqlite-amalgamation-$(SQLITE_VER).zip
SQLITE_URL = https://www.sqlite.org/$(SQLITE_YEAR)/$(SQLITE_ZIP)
SQLITE_DIR = lib/sqlite3

# Detect OS
UNAME_S := $(shell uname -s)
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
    CFLAGS += -DCWIST_OS_BSD
    IO_SRC = src/sys/io/kqueue.c
endif
ifeq ($(UNAME_S),FreeBSD)
    CFLAGS += -DCWIST_OS_BSD
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
lib/sqlite3/sqlite3.o: CFLAGS := $(filter-out -Werror,$(CFLAGS))

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
       src/net/http/curl_global.c \
       src/net/http/http_client.c \
       src/net/http/http3_client.c \
       src/net/http/https.c \
       src/net/http/tls_chain.c \
       src/net/grpc/grpc.c \
       src/net/grpc/protobuf.c \
       src/https/pqc_layer.c \
       src/net/http/mux.c \
       src/net/http/query.c \
       src/sys/session/session_manager.c \
       src/sys/app/csrf.c \
       src/sys/app/waf.c \
       src/core/siphash/siphash.c \
       src/core/db/db.c \
       src/core/db/nuke_db.c \
       src/core/db/migrate.c \
       src/core/orm/orm.c \
       src/core/orm/orm_socket.c \
       src/core/orm/rdbms_auto_mount.c \
       src/sys/app/app.c \
       src/net/websocket/websocket.c \
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
       src/core/log/log.c \
       src/sys/session/flash.c \
       src/core/template/template.c \
       src/core/html/builder.c \
       src/core/html/css_composer.c \
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

OBJS = $(SRCS:.c=.o)
LIB_NAME = libcwist.a

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
DEPSDIR ?= $(LIBDIR)/cwist
VENDOR_INCLUDEDIR ?= $(INCLUDEDIR)/cwist/vendor

INSTALL_LIBDIR = $(DESTDIR)$(LIBDIR)
INSTALL_INCLUDEDIR = $(DESTDIR)$(INCLUDEDIR)
INSTALL_DEPSDIR = $(DESTDIR)$(DEPSDIR)
INSTALL_VENDOR_INCLUDEDIR = $(DESTDIR)$(VENDOR_INCLUDEDIR)

EXTERNAL_LIBS = $(URIPARSER_LIB) \
                $(CJSON_LIB) \
                $(LIBTTAK_LIB) \
                $(CNATS_LIB) \
                $(LSQUIC_LIB) \
                $(BORINGSSL_SSL_LIB) \
                $(BORINGSSL_CRYPTO_LIB)

all: $(LIB_NAME)

$(LIB_NAME): $(OBJS)
	ar rcs $@ $^

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
	$(MAKE) -C $(LIBTTAK_DIR)

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
		-DBUILD_SHARED_LIBS=OFF \
		-DLSQUIC_WEBTRANSPORT=ON
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

TEST_TARGETS = test_sstring \
               test_seq \
               test_seq_auth \
               test_sha256 \
               test_error \
               test_inline_exports \
               test_arena \
               test_wasm_stream \
               test_stream_producer \
               test_healthz \
               test_json_builder \
               test_flash \
               test_middleware_jwt \
               test_sse \
               test_graphql \
               test_graphql_subscriptions \
               test_core_hardening \
               test_https_full_gc \
               test_cwist \
               test_html_builder \
               test_html_component \
               test_html_response \
               test_assets \
               test_html_parity \
               test_http2_flow_control \
               test_idle_reaper \
               test_linux_writer_fast \
               test_orm_socket \
               test_webtransport \
               test_http \
               test_siphash \
               test_mux \
               test_mux_param \
               test_rdbms_auto_mount \
               stress_test \
               test_cors \
               test_websocket \
               test_jwt \
               test_migrate \
               test_json_heal \
               test_https \
               test_http2 \
               test_http3 \
               test_shutdown \
               test_compress \
               test_log \
               nuke_missing_user_test \
               test_bind \
               test_metrics \
               test_io_uring \
               test_access_log \
               test_rate_limit \
               test_cache \
               test_bdr \
               test_secure_headers \
               test_http_chunked \
               test_static_and_range \
               test_session \
               test_csrf \
               test_db_pool \
               test_redis \
               test_scheduler \
               test_test_client \
               test_multiport \
               test_grpc

.PHONY: all test $(TEST_TARGETS) fuzz_seq install uninstall clean rebuild examples clean-examples

# Run with e.g. `make fuzz_seq FUZZ_RUNS=100000`.  The target intentionally
# uses a dedicated clang/libFuzzer toolchain and is not part of `make test`.
FUZZ_RUNS ?= 10000
fuzz_seq: $(LIBTTAK_LIB) $(CJSON_LIB) tests/fuzz_seq.c src/core/seq/seq.c src/core/seq/seq_auth.c src/core/mem/alloc.c
	$(FUZZ_CC) $(INCLUDE_PATHS) $(COMMON_DEFINES) -std=c2x -g -O1 \
		-fsanitize=fuzzer,address,undefined -o $@ tests/fuzz_seq.c \
		src/core/seq/seq.c src/core/seq/seq_auth.c src/core/mem/alloc.c \
		$(LIBTTAK_LIB) $(CJSON_LIB) $(BORINGSSL_SSL_LIB) $(BORINGSSL_CRYPTO_LIB) -pthread
	./$@ -runs=$(FUZZ_RUNS)

bench_security_pool: $(LIB_NAME) tests/bench_security_pool.c
	$(CC) $(CFLAGS) -o bench_security_pool tests/bench_security_pool.c $(LIB_NAME) $(LIBS)
	./bench_security_pool

test: $(TEST_TARGETS)

test_sstring: $(LIB_NAME) tests/test_sstring.c
	$(CC) $(CFLAGS) -o test_sstring tests/test_sstring.c $(LIB_NAME) $(LIBS)
	./test_sstring

test_websocket: $(LIB_NAME) tests/test_websocket.c
	$(CC) $(CFLAGS) -o test_websocket tests/test_websocket.c $(LIB_NAME) $(LIBS)
	./test_websocket

stress-test: $(LIB_NAME) tests/stress_test.c
	$(CC) $(CFLAGS) -o stress_test tests/stress_test.c $(LIB_NAME) $(LIBS)
	./stress_test

test_seq_auth: $(LIB_NAME) tests/test_seq_auth.c
	$(CC) $(CFLAGS) -o test_seq_auth tests/test_seq_auth.c $(LIB_NAME) $(LIBS)
	./test_seq_auth

test_sha256: tests/test_sha256.c include/cwist/core/crypto/sha256.h
	$(CC) $(CFLAGS) -o $@ tests/test_sha256.c
	./$@

test_error: $(LIB_NAME) tests/test_error.c
	$(CC) $(CFLAGS) -o test_error tests/test_error.c $(LIB_NAME) $(LIBS)
	./test_error

# Out-of-line wrappers for public static inline helpers: same results as the
# inline versions, and defined global symbols in the archive and in a program
# linked against it (what a binding generated from the headers links to).
INLINE_EXPORT_SYMBOLS = cwist_error_is_ok_extern cwist_endpoint_has_extern
test_inline_exports: $(LIB_NAME) tests/test_inline_exports.c
	$(CC) $(CFLAGS) -o test_inline_exports tests/test_inline_exports.c $(LIB_NAME) $(LIBS)
	./test_inline_exports
	@for sym in $(INLINE_EXPORT_SYMBOLS); do \
		for obj in $(LIB_NAME) test_inline_exports; do \
			nm -g $$obj 2>/dev/null | grep -Eq " T _?$$sym$$" || \
				{ echo "FAIL: $$sym is not a defined global symbol in $$obj"; exit 1; }; \
		done; \
		echo "Passed $$sym is a defined global symbol in $(LIB_NAME) and test_inline_exports"; \
	done

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

test_middleware_jwt: $(LIB_NAME) tests/test_middleware_jwt.c
	$(CC) $(CFLAGS) -o test_middleware_jwt tests/test_middleware_jwt.c $(LIB_NAME) $(LIBS)
	./test_middleware_jwt

test_http: $(LIB_NAME) tests/test_http.c
	$(CC) $(CFLAGS) -o test_http tests/test_http.c $(LIB_NAME) $(LIBS)
	./test_http

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

# Standalone h2c server for external conformance tools (h2spec); build-only,
# executed by the interop CI job, not by `make test`.
h2c-server: $(LIB_NAME) tests/h2c_server.c
	$(CC) $(CFLAGS) -o h2c-server tests/h2c_server.c $(LIB_NAME) $(LIBS)

test_http3: $(LIB_NAME) tests/test_http3.c
	$(CC) $(CFLAGS) -o test_http3 tests/test_http3.c $(LIB_NAME) $(LIBS)
	./test_http3

test_webtransport: $(LIB_NAME) tests/test_webtransport.c
	$(CC) $(CFLAGS) -o test_webtransport tests/test_webtransport.c $(LIB_NAME) $(LIBS)
	./test_webtransport

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

test_rate_limit: $(LIB_NAME) tests/test_rate_limit.c
	$(CC) $(CFLAGS) -o test_rate_limit tests/test_rate_limit.c $(LIB_NAME) $(LIBS)
	./test_rate_limit

test_cache: $(LIB_NAME) tests/test_cache.c
	$(CC) $(CFLAGS) -o test_cache tests/test_cache.c $(LIB_NAME) $(LIBS)
	./test_cache

test_bdr: $(LIB_NAME) tests/test_bdr.c
	$(CC) $(CFLAGS) -o test_bdr tests/test_bdr.c $(LIB_NAME) $(LIBS)
	./test_bdr

install: $(LIB_NAME)
	@echo "Installing CWIST library to $(LIBDIR)..."
	install -d $(INSTALL_LIBDIR)
	install -m 644 $(LIB_NAME) $(INSTALL_LIBDIR)/
	@echo "Installing external archives to $(DEPSDIR)..."
	install -d $(INSTALL_DEPSDIR)
	install -m 644 $(EXTERNAL_LIBS) $(INSTALL_DEPSDIR)/
	@echo "Installing CWIST headers to $(INCLUDEDIR)/cwist..."
	install -d $(INSTALL_INCLUDEDIR)/cwist
	cp -R include/cwist/. $(INSTALL_INCLUDEDIR)/cwist/
	install -d $(INSTALL_INCLUDEDIR)/wasm
	cp -R include/wasm/. $(INSTALL_INCLUDEDIR)/wasm/
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
	@echo "Uninstallation complete."

clean:
	@echo "Cleaning up build artifacts..."
	rm -f $(OBJS) $(LIB_NAME)
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

test_db_pool: $(LIB_NAME) tests/test_db_pool.c
	$(CC) $(CFLAGS) -o test_db_pool tests/test_db_pool.c $(LIB_NAME) $(LIBS)
	./test_db_pool

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

test_css_composer: $(LIB_NAME) tests/test_css_composer.c
	$(CC) $(CFLAGS) -o test_css_composer tests/test_css_composer.c $(LIB_NAME) $(LIBS)
	./test_css_composer

# The following seven had a tests/test_*.c file with no Makefile rule at
# all (so no CI coverage, ever) until scripts/ci/check_test_wiring.py
# caught the gap - all seven build and pass as-is, just never wired in.
test_cwist: $(LIB_NAME) tests/test_cwist.c
	$(CC) $(CFLAGS) -o test_cwist tests/test_cwist.c $(LIB_NAME) $(LIBS)
	./test_cwist

test_html_builder: $(LIB_NAME) tests/test_html_builder.c
	$(CC) $(CFLAGS) -o test_html_builder tests/test_html_builder.c $(LIB_NAME) $(LIBS)
	./test_html_builder

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

test_webtransport: $(LIB_NAME) tests/test_webtransport.c
	$(CC) $(CFLAGS) -o test_webtransport tests/test_webtransport.c $(LIB_NAME) $(LIBS)
	./test_webtransport


# ---------------------------------------------------------------------------
# Source formatting (clang-format; rules and their rationale in .clang-format)
#
# Scoped to the trees CWIST owns.  lib/ is vendored and carries its own
# .clang-format with DisableFormat, so it stays untouched either way.
# ---------------------------------------------------------------------------
FORMAT_DIRS := src include tests example benchmarks
FORMAT_FILES := $(shell find $(FORMAT_DIRS) -type f \( -name '*.c' -o -name '*.h' \) 2>/dev/null | sort)

.PHONY: format format-check

format:
	@clang-format -i $(FORMAT_FILES)
	@echo "clang-format: reformatted $(words $(FORMAT_FILES)) files in $(FORMAT_DIRS)"

format-check:
	@clang-format --dry-run --Werror $(FORMAT_FILES)
	@echo "clang-format: $(words $(FORMAT_FILES)) files already conform"
