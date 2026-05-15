CC = gcc
CFLAGS = -I./include -I./lib -I./lib/cjson -Wall -Wextra -pthread
LIBS = -pthread -lcjson -lssl -lcrypto -luriparser -lsqlite3

SRCS = src/core/sstring/sstring.c \
       src/sys/err/error.c \
       src/net/http/http.c \
       src/net/http/https.c \
       src/net/http/mux.c \
       src/net/http/query.c \
       src/sys/session/session_manager.c \
       src/core/siphash/siphash.c \
       src/core/db/db.c \
       src/sys/app/app.c \
       src/net/websocket/websocket.c \
       src/net/websocket/ws_utils.c \
       src/core/utils/json_builder.c \
       src/sys/app/middleware.c

OBJS = $(SRCS:.c=.o)
LIB_NAME = libcwist.a

PREFIX ?= /usr/local
LIBDIR = $(PREFIX)/lib
INCLUDEDIR = $(PREFIX)/include

all: $(LIB_NAME)

$(LIB_NAME): $(OBJS)
	ar rcs $@ $^

# SQLite Download & Extraction Rule
$(SQLITE_DIR)/sqlite3.c:
	@echo "Downloading SQLite..."
	@mkdir -p $(SQLITE_DIR)
	@wget -q $(SQLITE_URL) -O $(SQLITE_DIR)/$(SQLITE_ZIP)
	@echo "Extracting SQLite..."
	@unzip -q -j $(SQLITE_DIR)/$(SQLITE_ZIP) -d $(SQLITE_DIR)
	@rm $(SQLITE_DIR)/$(SQLITE_ZIP)
	@echo "SQLite Ready."

# Ensure lsquic submodule is checked out before compiling objects that need its headers
$(OBJS): | lib/lsquic/include/lsquic.h

lib/lsquic/include/lsquic.h:
	@if [ ! -f "$@" ]; then \
		echo "Initializing lsquic submodule..."; \
		git submodule update --init --recursive $(LSQUIC_DIR); \
	fi

$(LIB_NAME): $(URIPARSER_LIB) $(CJSON_LIB) $(LIBTTAK_LIB) $(CNATS_LIB) $(BORINGSSL_SSL_LIB) $(BORINGSSL_CRYPTO_LIB) $(LSQUIC_LIB) $(OBJS)
	@echo "Creating static library..."
	@rm -rf .lib_merge_tmp
	@mkdir -p .lib_merge_tmp
	@cd .lib_merge_tmp && \
		ar x $(abspath $(URIPARSER_LIB)) && \
		ar x $(abspath $(CJSON_LIB)) && \
		ar x $(abspath $(LIBTTAK_LIB)) && \
		ar x $(abspath $(CNATS_LIB)) && \
		ar x $(abspath $(LSQUIC_LIB)) && \
		ar x $(abspath $(BORINGSSL_SSL_LIB)) && \
		ar x $(abspath $(BORINGSSL_CRYPTO_LIB))
	ar rcs $@ $(OBJS) .lib_merge_tmp/*.o
	@rm -rf .lib_merge_tmp

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

TEST_TARGETS = test_sstring \
               test_http \
               test_siphash \
               test_mux \
               test_mux_param \
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
               test_cache

.PHONY: all test $(TEST_TARGETS) install uninstall clean rebuild

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

test_http: $(LIB_NAME) tests/test_http.c
	$(CC) $(CFLAGS) -o test_http tests/test_http.c $(LIB_NAME) $(LIBS)
	./test_http

test_siphash: $(LIB_NAME) tests/test_siphash.c
	$(CC) $(CFLAGS) -o test_siphash tests/test_siphash.c $(LIB_NAME) $(LIBS)
	./test_siphash

test_mux: $(LIB_NAME) tests/test_mux.c
	$(CC) $(CFLAGS) -o test_mux tests/test_mux.c $(LIB_NAME) $(LIBS)
	./test_mux

test_cors: $(LIB_NAME) tests/test_cors.c
	$(CC) $(CFLAGS) -o test_cors tests/test_cors.c $(LIB_NAME) $(LIBS)
	./test_cors

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

test_access_log: $(LIB_NAME) tests/test_access_log.c
	$(CC) $(CFLAGS) -o test_access_log tests/test_access_log.c $(LIB_NAME) $(LIBS)
	./test_access_log

test_rate_limit: $(LIB_NAME) tests/test_rate_limit.c
	$(CC) $(CFLAGS) -o test_rate_limit tests/test_rate_limit.c $(LIB_NAME) $(LIBS)
	./test_rate_limit

test_cache: $(LIB_NAME) tests/test_cache.c
	$(CC) $(CFLAGS) -o test_cache tests/test_cache.c $(LIB_NAME) $(LIBS)
	./test_cache

install: $(LIB_NAME)
	install -d $(LIBDIR)
	install -d $(INCLUDEDIR)/cwist
	install -d $(INCLUDEDIR)/cwist/err
	install -m 644 $(LIB_NAME) $(LIBDIR)
	install -m 644 include/cwist/*.h $(INCLUDEDIR)/cwist
	install -m 644 include/cwist/err/*.h $(INCLUDEDIR)/cwist/err

uninstall:
	rm -f $(LIBDIR)/$(LIB_NAME)
	rm -rf $(INCLUDEDIR)/cwist

clean:
	rm -f $(OBJS) $(LIB_NAME) test_sstring test_http test_siphash test_mux stress_test test_cors