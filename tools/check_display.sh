#!/usr/bin/env bash
# Pharos - prove the REAL panel path is in the shipped image.
#
# This exists because of a bug that shipped in v1.4.0 through v1.7.0 and cost
# days of hardware debugging. pharos_bsp.c selects between a real-panel path
# and a simulated one with
#
#     #elif !defined(CONFIG_PHAROS_HAS_VENDOR_BSP)
#
# but ESP-IDF does not force-include sdkconfig.h, and every ESP header in that
# file is included INSIDE the branches - after the test. So the macro was
# undefined at the moment it was tested, the simulated path was compiled every
# single time, and the panel was never touched. sdkconfig said y. The build
# honoured it everywhere except the one file that mattered.
#
# It compiled cleanly, linked cleanly, passed every other audit, and booted to
# a black screen. Only the device's own log gave it away. So: check the ELF.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2

RED=$'\033[31m'; GRN=$'\033[32m'; YEL=$'\033[33m'; RST=$'\033[0m'
ok()  { printf '%s  ok%s   %s\n' "$GRN" "$RST" "$*"; }
bad() { printf '%s  FAIL%s %s\n' "$RED" "$RST" "$*"; fail=1; }
fail=0

echo "[1] sdkconfig.h is included before any CONFIG_ test"
for f in components/pharos_bsp/pharos_bsp.c components/pharos_ui/pharos_hud.c; do
  [ -f "$f" ] || continue
  cfg_line=$(grep -n 'include "sdkconfig.h"' "$f" | head -n1 | cut -d: -f1)
  # Only real preprocessor directives count - the macro name also appears in
  # the explanatory comments, and those are not what selects the branch.
  test_line=$(grep -nE '^[[:space:]]*#[[:space:]]*(if|elif|ifdef|ifndef)\b.*CONFIG_PHAROS_HAS_VENDOR_BSP' "$f" | head -n1 | cut -d: -f1)
  if [ -z "$cfg_line" ]; then
    bad "$f tests a CONFIG_ macro but never includes sdkconfig.h"
  elif [ -n "$test_line" ] && [ "$cfg_line" -gt "$test_line" ]; then
    bad "$f includes sdkconfig.h (line $cfg_line) AFTER testing the macro (line $test_line)"
  else
    ok "$f"
  fi
done

ELF="${1:-}"
if [ -z "$ELF" ] || [ ! -f "$ELF" ]; then
  echo "[2] skipped (no ELF given; CI passes build/pharos.elf)"
  [ "$fail" -eq 0 ] && { printf '\n%sDISPLAY PATH OK%s (source check only)\n' "$GRN" "$RST"; exit 0; }
  printf '\n%sDISPLAY PATH BROKEN%s\n' "$RED" "$RST"; exit 1
fi

echo "[2] the shipped image drives the real panel: $ELF"
# The simulated path's warning string is the fingerprint. If it is in the
# image, that path was compiled, and the screen will be black.
if strings -a "$ELF" 2>/dev/null | grep -q "simulated board"; then
  bad "the SIMULATED board path is in this image - the panel will stay dark"
else
  ok "simulated path absent"
fi
# ... and the real path must actually be present.
if strings -a "$ELF" 2>/dev/null | grep -q "bsp_display_start"; then
  ok "vendor bring-up linked in"
else
  bad "no reference to bsp_display_start - the vendor BSP is not being called"
fi

if [ "$fail" -eq 0 ]; then
  printf '\n%sDISPLAY PATH OK%s - the real panel is driven.\n' "$GRN" "$RST"
  exit 0
fi
printf '\n%sDISPLAY PATH BROKEN%s - this image would boot to a black screen.\n' "$RED" "$RST"
exit 1
