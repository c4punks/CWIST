CC = gcc
CFLAGS = -Wall -Wextra
LIBS = -lcjson -lcwist

TARGET = simple-hashing

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) main.c $(LIBS) -o $(TARGET)

clean:
	rm -f $(TARGET)
