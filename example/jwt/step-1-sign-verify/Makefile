CC      = gcc
CFLAGS  = -Wall -Wextra -g -I../../../include -I../../../lib -I../../../lib/libttak/include
LDFLAGS = -L../../../ -L../../../lib/cjson -L../../../lib/libttak/lib -L../../../lib/boringssl/build
LIBS    = -lcwist -lcjson -lssl -lcrypto -lttak -lpthread -ldl -lstdc++ -lm -lz

TARGET = step-1-sign-verify

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) main.c $(LDFLAGS) $(LIBS) -o $(TARGET)

clean:
	rm -f $(TARGET)
