# Compiler and Flags
CC = gcc
# Added -g for debug info, -O2 for optimization (optional, adjust as needed)
CFLAGS = -I./include -I./lib -I./lib/cjson -Wall -Wextra -pthread -g
LIBS = -pthread -lcjson -lssl -lcrypto -luriparser -lsqlite3

# Source Files
SRCS = src/core/sstring/sstring.c \
       src/core/seq/seq.c \
       src/core/seq/seq_auth.c \
       src/sys/err/error.c \
       src/net/http/http.c \
       src/net/http/sse.c \
       src/net/graphql/graphql.c \
       src/net/graphql/graphql_ws.c \
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
       src/net/http/async.c src/core/mem/gc.c lib/libttak/src/mem/epoch.c src/sys/sys_info.c \
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
       src/sys/app/middleware.c \
       src/core/template/template.c \
       src/core/html/builder.c

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
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
PCDIR ?= $(LIBDIR)/pkgconfig
DEPSDIR ?= $(LIBDIR)/cwist

# --- Build Targets ---

all: $(LIB_NAME)

$(LIB_NAME): $(OBJS)
	@echo "Creating static library..."
	ar rcs $@ $^

# --- Test Targets ---

test: $(LIB_NAME) tests/test_sstring.c
	$(CC) $(CFLAGS) -o test_sstring tests/test_sstring.c $(LIB_NAME) $(LIBS)
	./test_sstring

# Struct layout contract for bindings (tests/abi_layout.h). The checks are
# compile-time; the binary prints the layout and compares the legacy-header
# view. It needs no library code, only the headers $(LIB_NAME) prepares.
ABI_LAYOUT_SRCS = tests/test_abi_layout.c tests/abi_layout_legacy.c
test_abi_layout: $(LIB_NAME) $(ABI_LAYOUT_SRCS) tests/abi_layout.h
	$(CC) $(CFLAGS) -o test_abi_layout $(ABI_LAYOUT_SRCS)
	./test_abi_layout

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

test_middleware_jwt: $(LIB_NAME) tests/test_middleware_jwt.c
	$(CC) $(CFLAGS) -o test_middleware_jwt tests/test_middleware_jwt.c $(LIB_NAME) $(LIBS)
	./test_middleware_jwt

# Out-of-line wrappers for public static inline helpers: same results as the
# inline versions, and defined global symbols in the archive and in a program
# linked against it (what a binding generated from the headers links to).
INLINE_EXPORT_SYMBOLS = cwist_error_is_ok_extern cwist_endpoint_has_extern
test_inline_exports: $(LIB_NAME) tests/test_inline_exports.c
	$(CC) $(CFLAGS) -rdynamic -o test_inline_exports tests/test_inline_exports.c $(LIB_NAME) $(LIBS)
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

test_https_park: $(LIB_NAME) tests/test_https_park.c
	$(CC) $(CFLAGS) -o test_https_park tests/test_https_park.c $(LIB_NAME) $(LIBS)
	./test_https_park

test_https_metrics: $(LIB_NAME) tests/test_https_metrics.c
	$(CC) $(CFLAGS) -o test_https_metrics tests/test_https_metrics.c $(LIB_NAME) $(LIBS)
	./test_https_metrics

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

# Pattern rule for object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# --- Install / Uninstall ---

install: $(LIB_NAME)
	@echo "Installing library to $(LIBDIR)..."
	install -d $(LIBDIR)
	install -m 644 $(LIB_NAME) $(LIBDIR)

	@echo "Installing headers to $(INCLUDEDIR)/cwist..."
	install -d $(INCLUDEDIR)/cwist

	# Recursively copy headers to preserve the directory structure
	# (e.g., include/cwist/net/http/http.h -> /usr/local/include/cwist/net/http/http.h)
	cp -r include/cwist/* $(INCLUDEDIR)/cwist/

	# Set correct permissions for the copied files and directories
	find $(INCLUDEDIR)/cwist -type d -exec chmod 755 {} \;
	find $(INCLUDEDIR)/cwist -type f -exec chmod 644 {} \;
	@echo "Installation complete."

uninstall:
	@echo "Uninstalling cwist..."
	rm -f $(LIBDIR)/$(LIB_NAME)
	rm -rf $(INCLUDEDIR)/cwist
	@echo "Uninstallation complete."

clean:
	@echo "Cleaning up build artifacts..."
	rm -f $(OBJS) $(LIB_NAME)
	rm -f test_sstring test_http test_siphash test_mux stress_test test_cors test_websocket
