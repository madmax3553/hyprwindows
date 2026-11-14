CC ?= cc
CXX ?= g++
MOC ?= $(shell qmake6 -query QT_INSTALL_LIBEXECS)/moc
QT_CFLAGS := $(shell pkg-config --cflags Qt6Widgets)
QT_LIBS := $(shell pkg-config --libs Qt6Widgets)

CFLAGS ?= -Wall -Wextra -Werror -O2 -fPIE
CXXFLAGS ?= -Wall -Wextra -Werror -O2 -fPIE -mno-direct-extern-access $(QT_CFLAGS)
LDLIBS ?= $(QT_LIBS)

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man/man1
DATADIR ?= $(PREFIX)/share/hyprwindows

BIN = hyprwindows

all: $(BIN)

$(BIN): unity.o src/window.o
	$(CXX) -o $@ unity.o src/window.o $(LDLIBS)

unity.o: unity.c src/*.c src/*.h
	$(CC) $(CFLAGS) -c -o $@ unity.c

src/window.moc: src/window.cpp
	$(MOC) $(QT_CFLAGS) -o $@ src/window.cpp

src/window.o: src/window.cpp src/window.moc src/*.h
	$(CXX) $(CXXFLAGS) -c -o $@ src/window.cpp

debug: CFLAGS = -Wall -Wextra -g -O0 -DDEBUG -fPIE
debug: CXXFLAGS = -Wall -Wextra -g -O0 -DDEBUG -fPIE -mno-direct-extern-access $(QT_CFLAGS)
debug: clean $(BIN)

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	install -d $(DESTDIR)$(MANDIR)
	install -m 644 doc/hyprwindows.1 $(DESTDIR)$(MANDIR)/hyprwindows.1
	install -d $(DESTDIR)$(DATADIR)
	install -m 644 data/appmap.json $(DESTDIR)$(DATADIR)/appmap.json

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -f $(DESTDIR)$(MANDIR)/hyprwindows.1
	rm -f $(DESTDIR)$(DATADIR)/appmap.json

TEST_SRCS = tests/roundtrip.c src/util.c src/rules.c src/hyprconf.c src/actions.c src/appmap.c src/hyprctl.c

tests/roundtrip: $(TEST_SRCS) src/*.h
	$(CC) -Wall -Wextra -Werror -O2 -I. -o $@ $(TEST_SRCS)

test: tests/roundtrip
	./tests/roundtrip

clean:
	rm -f $(BIN) tests/roundtrip unity.o src/window.o src/window.moc

.PHONY: all debug install uninstall clean test
