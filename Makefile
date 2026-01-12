CC ?= clang
CFLAGS ?= -Wall -Wextra -Werror -O2
LDLIBS ?= -lncurses

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man/man1

SRC = \
	src/main.c \
	src/rules.c \
	src/hyprctl.c \
	src/util.c \
	src/appmap.c \
	src/actions.c \
	src/ui.c \
	src/hyprconf.c \
	src/simplejson.c

OBJ = $(SRC:.c=.o)

BIN = hyprwindows

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

debug: CFLAGS = -Wall -Wextra -g -O0 -DDEBUG
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
	rm -f $(OBJ) $(BIN)

.PHONY: all debug install uninstall clean
