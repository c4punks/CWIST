CC = gcc
CFLAGS = -Wall -Wextra
LIBS = -lcjson -lcwist

SRCS = main.c
OBJS = $(SRCS:.c=.o)
TARGET = cde-viewer

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LDFLAGS) $(LIBS)

clean:
	rm -f $(TARGET)