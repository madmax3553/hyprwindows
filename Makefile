CC ?= cc
CFLAGS ?= -Wall -Wextra -Werror -O2
LDLIBS ?= -lncurses

SRC = \
	src/main.c \
	src/json.c \
	src/rules.c \
	src/hyprctl.c \
	src/util.c \
	src/appmap.c \
	src/actions.c \
	src/ui.c \
	third_party/jsmn.c

OBJ = $(SRC:.c=.o)

BIN = hyprwindows

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean
