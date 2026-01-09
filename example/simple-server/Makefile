CC = gcc
CFLAGS = -I../../include -I../../lib -Wall -Wextra -g
LIBS = -lcjson

# Path to library sources
SRC_DIR = ../../src
SRCS = $(SRC_DIR)/smartstring/smartstring.c \
       $(SRC_DIR)/process/err/error.c \
       $(SRC_DIR)/http/http.c \
       main.c

TARGET = simple_server

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LIBS)

clean:
	rm -f $(TARGET)
