CC ?= cc
CFLAGS ?= -Wall -Wextra -Werror -O2 $(shell pkg-config --cflags notcurses-core)
LDLIBS ?= $(shell pkg-config --libs notcurses-core)

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man/man1

BIN = hyprwindows

all: $(BIN)

$(BIN): unity.c src/*.c src/*.h
	$(CC) $(CFLAGS) -o $@ unity.c $(LDLIBS)

debug: CFLAGS = -Wall -Wextra -g -O0 -DDEBUG $(shell pkg-config --cflags notcurses-core)
debug: clean $(BIN)

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	install -d $(DESTDIR)$(MANDIR)
	install -m 644 doc/hyprwindows.1 $(DESTDIR)$(MANDIR)/hyprwindows.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -f $(DESTDIR)$(MANDIR)/hyprwindows.1

clean:
	rm -f $(BIN)

.PHONY: all debug install uninstall clean
