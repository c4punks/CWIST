CC = gcc
CFLAGS = -I./include -I./lib -I./lib/cjson -Wall -Wextra -pthread
LIBS = -pthread -lcjson

SRCS = src/sstring/sstring.c src/process/err/error.c src/http/http.c src/session/session_manager.c
OBJS = $(SRCS:.c=.o)
LIB_NAME = libcwist.a

PREFIX ?= /usr/local
LIBDIR = $(PREFIX)/lib
INCLUDEDIR = $(PREFIX)/include

all: $(LIB_NAME)

$(LIB_NAME): $(OBJS)
	ar rcs $@ $^

test: $(LIB_NAME) tests/test_sstring.c
	$(CC) $(CFLAGS) -o test_sstring tests/test_sstring.c $(LIB_NAME) $(LIBS)
	./test_sstring

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

test_http3: $(LIB_NAME) tests/test_http3.c
	$(CC) $(CFLAGS) -o test_http3 tests/test_http3.c $(LIB_NAME) $(LIBS)
	./test_http3

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

test_listen_ex: $(LIB_NAME) tests/test_listen_ex.c
	$(CC) $(CFLAGS) -o test_listen_ex tests/test_listen_ex.c $(LIB_NAME) $(LIBS)
	./test_listen_ex

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

test_io_uring: $(LIB_NAME) tests/test_io_uring.c
	$(CC) $(CFLAGS) -o test_io_uring tests/test_io_uring.c $(LIB_NAME) $(LIBS)
	./test_io_uring

test_io_uring_demolition: $(LIB_NAME) tests/test_io_uring_demolition.c
	$(CC) $(CFLAGS) -o test_io_uring_demolition tests/test_io_uring_demolition.c $(LIB_NAME) $(LIBS)
	./test_io_uring_demolition

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

	@echo "Installing headers to $(INCLUDEDIR)/cwist..."
	install -d $(INCLUDEDIR)/cwist
	
	# Copy all headers including vendor (sqlite3)
	cp -r include/cwist/* $(INCLUDEDIR)/cwist/

	# Set correct permissions
	find $(INCLUDEDIR)/cwist -type d -exec chmod 755 {} \;
	find $(INCLUDEDIR)/cwist -type f -exec chmod 644 {} \;
	@echo "Installation complete."

uninstall:
	@echo "Uninstalling cwist..."
	rm -f $(LIBDIR)/$(LIB_NAME)
	rm -rf $(INCLUDEDIR)/cwist
	@echo "Uninstallation complete."

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
	rm -f $(OBJS) $(LIB_NAME) test_sstring test_http
