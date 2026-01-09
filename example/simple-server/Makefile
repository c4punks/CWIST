CC = gcc
CFLAGS = -Wall -Wextra -g
LIBS = -lcwist -lcjson

SRCS = main.c
TARGET = simple_server

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LIBS)

clean:
	rm -f $(TARGET)
