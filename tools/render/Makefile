# Virtual Pharos - render the round HUD on a laptop, from the real firmware
# geometry and the real detection engines.
#
#   make            build, render, write assets/screens/*.png
#   make check      bounds-check only (no PNGs, no Python needed)
#
# `check` is what CI runs: it asserts that no primitive escapes the panel's
# safe radius, which makes "does the UI fit on a circle" a test result.

ROOT := ../..
CC ?= cc
CFLAGS ?= -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter
INCLUDES := \
  -I$(ROOT)/components/pharos_core/include \
  -I$(ROOT)/components/pharos_engine/include \
  -I$(ROOT)/components/pharos_ui/include

SRCS := \
  pharos_render.c \
  $(ROOT)/components/pharos_engine/pharos_census.c \
  $(ROOT)/components/pharos_engine/pharos_console.c \
  $(ROOT)/components/pharos_engine/pharos_flood.c \
  $(ROOT)/components/pharos_engine/pharos_karma.c \
  $(ROOT)/components/pharos_engine/pharos_locate.c \
  $(ROOT)/components/pharos_engine/pharos_opsec.c \
  $(ROOT)/components/pharos_engine/pharos_range.c \
  $(ROOT)/components/pharos_engine/pharos_twin.c \
  $(ROOT)/components/pharos_engine/pharos_watch.c \
  $(ROOT)/components/pharos_ui/pharos_dial.c \
  $(ROOT)/components/pharos_ui/pharos_round.c

BIN := pharos_render
OUT := $(ROOT)/assets/screens

.PHONY: all render gallery check clean
all: render gallery

$(BIN): $(SRCS)
	$(CC) $(CFLAGS) $(INCLUDES) $(SRCS) -lm -o $@

render: $(BIN)
	./$(BIN) | python3 rasterize.py $(OUT)

gallery: render
	python3 gallery.py $(OUT) $(ROOT)/assets/branding/gallery.png

check: $(BIN)
	./$(BIN) --check

clean:
	rm -rf $(BIN) $(BIN).dSYM
