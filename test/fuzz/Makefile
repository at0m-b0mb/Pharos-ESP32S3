# Pharos - the fuzz harness for the parsers that read hostile input.
#
# Built with ASan and UBSan ON BY DEFAULT, because without them this only
# proves the parsers return - the sanitizers are what detect the fault. A
# build without them is almost pointless, so it is not the default.
#
#   make            # a short run of both, what CI does
#   make long       # a million iterations
#   make replay SEED=12345         # one input, for a reported failure
#   make replay-state SEED=12345   # one verdict stream, likewise
#
# There are two harnesses. fuzz.c drives the PARSERS with hostile bytes;
# fuzz_state.c drives the SCORING ENGINES with hostile event streams and
# checks that the verdicts stay honest - that no number ever exceeds the
# ceiling its evidence earned.
#
# The seed sequence is deterministic: the same seed gives the same bytes on
# every machine, so a failure is replayable with one number rather than with a
# corpus file.

ROOT := ../..

CFLAGS ?= -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
          -fno-strict-aliasing \
          -fsanitize=undefined,address -fno-sanitize-recover=all \
          -I$(ROOT)/components/pharos_core/include \
          -I$(ROOT)/components/pharos_engine/include

# Only the pure engines. Anything that includes esp_*.h cannot build here, and
# does not need to: the parsers are deliberately free of the platform.
SRCS := fuzz.c \
  $(ROOT)/components/pharos_engine/pharos_dot11.c \
  $(ROOT)/components/pharos_engine/pharos_wps.c \
  $(ROOT)/components/pharos_engine/pharos_vigil.c \
  $(ROOT)/components/pharos_engine/pharos_roster.c

HDRS := $(wildcard $(ROOT)/components/pharos_engine/include/*.h) \
        $(wildcard $(ROOT)/components/pharos_core/include/*.h)

# The scoring engines, for the verdict harness.
STATE_SRCS := fuzz_state.c \
  $(ROOT)/components/pharos_engine/pharos_aegis.c \
  $(ROOT)/components/pharos_engine/pharos_census.c \
  $(ROOT)/components/pharos_engine/pharos_twin.c \
  $(ROOT)/components/pharos_engine/pharos_flood.c \
  $(ROOT)/components/pharos_engine/pharos_vigil.c

ITERS ?= 200000
STATE_ITERS ?= 50000
SEED  ?= 1

.PHONY: all long replay replay-state clean

all: fuzz fuzz_state
	./fuzz $(ITERS) $(SEED)
	./fuzz_state $(STATE_ITERS) $(SEED)

long: fuzz fuzz_state
	./fuzz 1000000 $(SEED)
	./fuzz_state 250000 $(SEED)

replay: fuzz
	./fuzz 1 $(SEED)

replay-state: fuzz_state
	./fuzz_state 1 $(SEED)

fuzz: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) $(SRCS) -lm -o $@

fuzz_state: $(STATE_SRCS) $(HDRS)
	$(CC) $(CFLAGS) $(STATE_SRCS) -lm -o $@

clean:
	rm -rf fuzz fuzz_state fuzz.dSYM fuzz_state.dSYM
