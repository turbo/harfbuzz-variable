# --- configurable bits -------------------------------------------------
CC          ?= clang            # or gcc
CFLAGS      ?= -O2 -Wall -std=c17
PKG_CONFIG  ?= pkg-config
SRC         := main.c
BIN         := demo_sdl3

# --- derive compiler & linker flags from pkg-config --------------------
PC_PKGS     := harfbuzz freetype2 sdl3
CFLAGS      += $(shell $(PKG_CONFIG) --cflags $(PC_PKGS))
LDLIBS      := $(shell $(PKG_CONFIG) --libs   $(PC_PKGS))

# --- rules -------------------------------------------------------------
all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)

.PHONY: all run clean
