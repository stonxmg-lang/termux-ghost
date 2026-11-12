PREFIX ?= /data/data/com.termux/files/usr
LIBDIR ?= $(PREFIX)/lib/bash
CFLAGS ?= -O2 -Wall -Wextra
TARGET = ghost.so

# CPPFLAGS/CFLAGS/LDFLAGS from the environment are respected (the Termux
# package builder sets them); -fPIC -shared are always added.
$(TARGET): ghost.c
	$(CC) $(CPPFLAGS) -I$(PREFIX)/include $(CFLAGS) -fPIC -shared -o $@ $< \
		-L$(PREFIX)/lib $(LDFLAGS) -lreadline -lhistory

# Installs where bash keeps loadable builtins: enable -f $PREFIX/lib/bash/ghost ghost
install: $(TARGET)
	install -Dm644 $(TARGET) $(DESTDIR)$(LIBDIR)/ghost

clean:
	rm -f $(TARGET)

.PHONY: install clean
