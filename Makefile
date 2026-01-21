CC = gcc
CFLAGS = -I./include -I./lib -I./lib/cjson -Wall -Wextra -pthread
LIBS = -pthread -lcjson -lssl -lcrypto -luriparser -lsqlite3

SRCS = src/sstring/sstring.c src/process/err/error.c src/http/http.c src/http/https.c src/http/mux.c src/http/query.c src/session/session_manager.c src/siphash/siphash.c src/db/db.c src/framework/app.c
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

test_siphash: $(LIB_NAME) tests/test_siphash.c
	$(CC) $(CFLAGS) -o test_siphash tests/test_siphash.c $(LIB_NAME) $(LIBS)
	./test_siphash

test_mux: $(LIB_NAME) tests/test_mux.c
	$(CC) $(CFLAGS) -o test_mux tests/test_mux.c $(LIB_NAME) $(LIBS)
	./test_mux

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

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
	rm -f $(OBJS) $(LIB_NAME) test_sstring test_http test_siphash test_mux
