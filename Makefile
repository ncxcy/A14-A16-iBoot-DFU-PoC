CC = cc
CFLAGS = -Wall -Wextra -std=gnu11 -O2 -Iinclude -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lusb-1.0 -lpthread
TARGET = poc
SRC = main.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) $(LDFLAGS) -o $(TARGET)

clean:
	rm -f $(TARGET)
