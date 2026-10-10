CC ?= gcc
CWIST_ROOT ?= ../..
CFLAGS ?= -Wall -Wextra -O2 -I$(CWIST_ROOT)/include -I$(CWIST_ROOT)/lib/boringssl/include -I$(CWIST_ROOT)/lib/cjson -I$(CWIST_ROOT)/lib/libttak/include -I$(CWIST_ROOT)/lib/uriparser/include -I$(CWIST_ROOT)/lib/lsquic/include -I$(CWIST_ROOT)/lib/sqlite3

LIBS = $(CWIST_ROOT)/libcwist.a \
       $(CWIST_ROOT)/lib/cnats/build/lib/libnats_static.a \
       $(CWIST_ROOT)/lib/libttak/lib/libttak.a \
       $(CWIST_ROOT)/lib/cjson/libcjson.a \
       $(CWIST_ROOT)/lib/uriparser/build/liburiparser.a \
       $(CWIST_ROOT)/lib/lsquic/build/src/liblsquic/liblsquic.a \
       $(CWIST_ROOT)/lib/boringssl/build/libssl.a \
       $(CWIST_ROOT)/lib/boringssl/build/libcrypto.a \
       -lsqlite3 -lcurl -pthread -ldl -lm -lstdc++ -lz

SRCS = src/main.c src/db.c src/handlers.c src/utils.c
OBJS = $(SRCS:.c=.o)
TARGET = server

all: $(TARGET)

$(TARGET): $(OBJS) $(CWIST_ROOT)/libcwist.a
	$(CC) $(OBJS) -o $(TARGET) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
