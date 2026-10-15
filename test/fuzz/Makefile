# Pharos - the fuzz harness for the parsers that read hostile input.
#
# Built with ASan and UBSan ON BY DEFAULT, because without them this only
# proves the parsers return - the sanitizers are what detect the fault. A
# build without them is almost pointless, so it is not the default.
#
#   make            # a short run, what CI does
#   make long       # a million iterations
#   make replay SEED=12345   # one input, for a reported failure
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

ITERS ?= 200000
SEED  ?= 1

.PHONY: all long replay clean

all: fuzz
	./fuzz $(ITERS) $(SEED)

long: fuzz
	./fuzz 1000000 $(SEED)

replay: fuzz
	./fuzz 1 $(SEED)

fuzz: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) $(SRCS) -lm -o $@

clean:
	rm -rf fuzz fuzz.dSYM
