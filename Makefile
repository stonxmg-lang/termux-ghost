PREFIX ?= /data/data/com.termux/files/usr
CC ?= clang
CFLAGS ?= -O2 -Wall -Wextra -fPIC
TARGET = ghost.so

$(TARGET): ghost.c
	$(CC) $(CFLAGS) -I$(PREFIX)/include -shared -o $@ $< -L$(PREFIX)/lib -lreadline -lhistory

clean:
	rm -f $(TARGET)

.PHONY: clean
