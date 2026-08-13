# Host test build. No ESP-IDF, no toolchain, no board.
#
#   make            build and run
#   make clean
#
# Everything under test here is deliberately free of ESP-IDF: the detection
# engines, the ingest ring and the round-screen geometry are plain C, so they
# can be developed and regression-tested on a laptop and only then flashed.

ROOT := ../..
CC ?= cc
CFLAGS ?= -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
          -fno-strict-aliasing
INCLUDES := \
  -I$(ROOT)/components/pharos_core/include \
  -I$(ROOT)/components/pharos_engine/include \
  -I$(ROOT)/components/pharos_radio/include \
  -I$(ROOT)/components/pharos_ui/include

SRCS := \
  test_pharos.c \
  test_engines.c \
  test_privacy.c \
  test_region.c \
  test_range.c \
  test_chain.c \
  test_redteam.c \
  $(ROOT)/components/pharos_core/pharos_bus.c \
  $(ROOT)/components/pharos_core/pharos_caps.c \
  $(ROOT)/components/pharos_core/pharos_lens.c \
  $(ROOT)/components/pharos_engine/pharos_census.c \
  $(ROOT)/components/pharos_engine/pharos_chain.c \
  $(ROOT)/components/pharos_engine/pharos_dot11.c \
  $(ROOT)/components/pharos_engine/pharos_flood.c \
  $(ROOT)/components/pharos_engine/pharos_karma.c \
  $(ROOT)/components/pharos_engine/pharos_power.c \
  $(ROOT)/components/pharos_engine/pharos_opsec.c \
  $(ROOT)/components/pharos_engine/pharos_probe.c \
  $(ROOT)/components/pharos_engine/pharos_range.c \
  $(ROOT)/components/pharos_engine/pharos_report.c \
  $(ROOT)/components/pharos_engine/pharos_sha256.c \
  $(ROOT)/components/pharos_engine/pharos_twin.c \
  $(ROOT)/components/pharos_engine/pharos_watch.c \
  $(ROOT)/components/pharos_ui/pharos_dial.c \
  $(ROOT)/components/pharos_radio/pharos_region.c \
  $(ROOT)/components/pharos_ui/pharos_round.c

BIN := test_pharos

.PHONY: all run clean
all: run

$(BIN): $(SRCS)
	$(CC) $(CFLAGS) $(INCLUDES) $(SRCS) -lm -o $@

run: $(BIN)
	./$(BIN)

clean:
	rm -rf $(BIN) $(BIN).dSYM
