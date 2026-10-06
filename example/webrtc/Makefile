CC ?= gcc
CFLAGS = -Wall -Wextra -g -I../../include

ROOT = ../..
LIBS = $(ROOT)/libcwist.a \
       $(ROOT)/lib/cnats/build/lib/libnats_static.a \
       $(ROOT)/lib/libttak/lib/libttak.a \
       $(ROOT)/lib/cjson/libcjson.a \
       $(ROOT)/lib/uriparser/build/liburiparser.a \
       $(ROOT)/lib/lsquic/build/src/liblsquic/liblsquic.a \
       $(ROOT)/lib/usrsctp/build/usrsctplib/libusrsctp.a \
       $(ROOT)/lib/boringssl/build/libssl.a \
       $(ROOT)/lib/boringssl/build/libcrypto.a \
       -pthread -ldl -lm -lstdc++ -lz -lcurl -lnghttp2

SRCS = main.c
TARGET = webrtc-server

all: $(TARGET)

$(TARGET): $(SRCS) $(ROOT)/libcwist.a
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LIBS)

clean:
	rm -f $(TARGET)
