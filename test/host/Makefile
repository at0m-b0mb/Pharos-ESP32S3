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
  -I$(ROOT)/components/pharos_ui/include \
  -I$(ROOT)/components/pharos_engine/include

SRCS := \
  test_pharos.c \
  test_engines.c \
  test_privacy.c \
  test_region.c \
  test_range.c \
  test_chain.c \
  test_redteam.c \
  test_locate.c \
  test_console.c \
  test_sentinel.c \
  test_harvest.c \
  test_aegis.c \
  test_squall.c \
  test_vigil.c \
  test_watch.c \
  test_rival.c \
  test_acoustic.c test_attrib.c test_pulse.c \
  test_theme.c \
  test_style.c \
  test_tower.c \
  test_survey.c \
  test_ring.c \
  test_motion.c \
  test_roster.c \
  test_wps.c \
  $(ROOT)/components/pharos_ui/pharos_theme.c \
  $(ROOT)/components/pharos_ui/pharos_style.c \
  $(ROOT)/components/pharos_engine/pharos_tower.c \
  $(ROOT)/components/pharos_engine/pharos_survey.c \
  $(ROOT)/components/pharos_engine/pharos_motion.c \
  $(ROOT)/components/pharos_engine/pharos_roster.c \
  $(ROOT)/components/pharos_engine/pharos_wps.c \
  $(ROOT)/components/pharos_core/pharos_bus.c \
  $(ROOT)/components/pharos_core/pharos_caps.c \
  $(ROOT)/components/pharos_core/pharos_lens.c \
  $(ROOT)/components/pharos_engine/pharos_acoustic.c \
  $(ROOT)/components/pharos_engine/pharos_attrib.c \
  $(ROOT)/components/pharos_engine/pharos_pulse.c \
  $(ROOT)/components/pharos_engine/pharos_aegis.c \
  $(ROOT)/components/pharos_engine/pharos_census.c \
  $(ROOT)/components/pharos_engine/pharos_console.c \
  $(ROOT)/components/pharos_engine/pharos_chain.c \
  $(ROOT)/components/pharos_engine/pharos_dot11.c \
  $(ROOT)/components/pharos_engine/pharos_flood.c \
  $(ROOT)/components/pharos_engine/pharos_harvest.c \
  $(ROOT)/components/pharos_engine/pharos_karma.c \
  $(ROOT)/components/pharos_engine/pharos_locate.c \
  $(ROOT)/components/pharos_engine/pharos_power.c \
  $(ROOT)/components/pharos_engine/pharos_opsec.c \
  $(ROOT)/components/pharos_engine/pharos_probe.c \
  $(ROOT)/components/pharos_engine/pharos_range.c \
  $(ROOT)/components/pharos_engine/pharos_report.c \
  $(ROOT)/components/pharos_engine/pharos_rival.c \
  $(ROOT)/components/pharos_engine/pharos_sentinel.c \
  $(ROOT)/components/pharos_engine/pharos_sha256.c \
  $(ROOT)/components/pharos_engine/pharos_squall.c \
  $(ROOT)/components/pharos_engine/pharos_twin.c \
  $(ROOT)/components/pharos_engine/pharos_vigil.c \
  $(ROOT)/components/pharos_engine/pharos_watch.c \
  $(ROOT)/components/pharos_ui/pharos_dial.c \
  $(ROOT)/components/pharos_radio/pharos_region.c \
  $(ROOT)/components/pharos_ui/pharos_round.c

BIN := test_pharos

.PHONY: all run clean
all: run

# HEADERS ARE PREREQUISITES TOO.
#
# This rule used to list only $(SRCS), so editing a header did NOT rebuild -
# make saw every .c file older than the binary and did nothing. A rename in
# pharos_style.h then "passed" locally against a stale binary and failed in CI,
# which is the worst possible order to find out. Everything under the audited
# include dirs is a prerequisite now; a full rebuild here takes seconds.
HEADERS := $(wildcard \
  $(ROOT)/components/*/include/*.h \
  $(ROOT)/test/host/*.h)

$(BIN): $(SRCS) $(HEADERS)
	$(CC) $(CFLAGS) $(INCLUDES) $(SRCS) -lm -o $@

run: $(BIN)
	./$(BIN)

clean:
	rm -rf $(BIN) $(BIN).dSYM
