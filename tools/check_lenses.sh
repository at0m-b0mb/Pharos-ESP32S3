#!/usr/bin/env bash
# Pharos - lens linkage audit
#
# Why this exists, and it is worth reading before deleting it.
#
# A lens registers itself from a constructor. Nothing in the project holds a
# reference to its translation unit, so the linker is entitled to discard the
# whole object - and it does, silently. The build stays green, the image is
# smaller, and the device boots with the lens simply *absent*. That failure
# mode already happened once in this repo: the firmware linked clean while
# every lens was missing, because an `INTERFACE --whole-archive` on the
# component library is not enough in ESP-IDF's link model. The fix is the
# WHOLE_ARCHIVE argument to idf_component_register.
#
# Since the symptom is "everything passes and the product is empty", the only
# defence is to look inside the linked image and insist each lens is really
# there. Lens ids are string literals in the descriptor, so they survive into
# .rodata and `strings` finds them.
#
#   tools/check_lenses.sh build/pharos.elf
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2

RED=$'\033[31m'; GRN=$'\033[32m'; RST=$'\033[0m'
ok()  { printf '%s  ok%s   %s\n' "$GRN" "$RST" "$*"; }
bad() { printf '%s  FAIL%s %s\n' "$RED" "$RST" "$*"; fail=1; }
fail=0

echo "Pharos lens linkage audit"
echo "========================="

# Source of truth: the ids declared in the lens sources themselves, so adding
# a lens extends this check automatically.
IDS=$(grep -rhoE '^\s*\.id\s*=\s*"[^"]+"' components/pharos_lens_*/ 2>/dev/null \
      | sed -E 's/.*"([^"]+)".*/\1/' | sort -u)

if [ -z "$IDS" ]; then
  bad "no lens ids found in components/pharos_lens_*/ - has the layout changed?"
  exit 1
fi

echo
echo "[1] every lens component declares WHOLE_ARCHIVE"
for d in components/pharos_lens_*/; do
  name=$(basename "$d")
  if grep -q "WHOLE_ARCHIVE" "$d/CMakeLists.txt" 2>/dev/null; then
    ok "$name"
  else
    bad "$name is missing WHOLE_ARCHIVE - its registration will be discarded"
  fi
done

echo
echo "[2] every lens is listed in main/CMakeLists.txt"
for d in components/pharos_lens_*/; do
  name=$(basename "$d")
  if grep -q "\b${name}\b" main/CMakeLists.txt 2>/dev/null; then
    ok "$name"
  else
    bad "$name is not in main's REQUIRES - it will not be built into the app"
  fi
done

ELF="${1:-}"
if [ -n "$ELF" ] && [ -f "$ELF" ]; then
  echo
  echo "[3] every lens id is present in the linked image: $ELF"

  # Read the image ONCE.
  #
  # This used to be `strings "$ELF" | grep -qxF "$id"` inside the loop, and
  # under `set -o pipefail` that is a trap: grep -q exits the instant it finds
  # a match, strings then dies of SIGPIPE with status 141, and pipefail
  # promotes 141 to the status of the whole pipeline. The `if` takes the else
  # branch and the audit reports a lens as DISCARDED while it is sitting right
  # there in the image.
  #
  # It is timing-dependent - whether strings has finished writing before grep
  # gives up on it varies with the file, the platform and the pipe buffer -
  # which is the worst possible property for an audit: it passed in CI and
  # failed on a developer's machine on the same commit. Reading once into a
  # variable removes the pipeline, and incidentally replaces sixteen passes
  # over a 15 MB ELF with one.
  #
  # The failure was in the safe direction here (a false FAIL blocks a release
  # rather than shipping a broken one) and pipefail can only ever make a
  # status MORE non-zero, so no audit in this repo could have false-PASSED
  # from it. It still had to go.
  ELF_STRINGS=$(strings -a "$ELF" 2>/dev/null)
  for id in $IDS; do
    if grep -qxF "$id" <<< "$ELF_STRINGS"; then
      ok "$id"
    else
      bad "$id is NOT in the image - the lens was discarded at link time"
    fi
  done
else
  echo
  echo "[3] skipped (no ELF given; CI passes build/pharos.elf)"
fi

# EVERY WATCH ON THE HOME RING IS A LENS THAT EXISTS.
#
# The ring is a hand-written list of lens ids in pharos_ui.c. A typo there does
# not break the build and does not crash: the ring simply carries one fewer
# watch, which is indistinguishable from a deliberate choice. "rf.rival" was
# written "ble.rival" and the only symptom was seven dots instead of eight.
echo
echo "[ring] every watch on the home ring resolves to a registered lens"
ring_bad=0
ring_ids=$(sed -n '/k_ring_order\[\] = {/,/};/p' components/pharos_ui/pharos_ui.c | \
           grep -oE '"[a-z0-9]+\.[a-z0-9]+"' | tr -d '"')
for rid in $ring_ids; do
  if ! grep -rqF "\"$rid\"" components/pharos_lens_*/*.c; then
    printf '%s  BAD%s  home ring names "%s", which no lens registers\n' "$RED" "$RST" "$rid"
    ring_bad=1
    fail=1
  fi
done
if [ "$ring_bad" -eq 0 ]; then
  printf '%s  ok%s   every ring watch resolves (%s)\n' "$GRN" "$RST" "$(echo $ring_ids | wc -w | tr -d ' ') watches"
fi

echo
if [ "$fail" -eq 0 ]; then
  printf '%sLENSES INTACT%s\n' "$GRN" "$RST"
  exit 0
else
  printf '%sLENS LINKAGE BROKEN%s - the device would boot missing tools.\n' "$RED" "$RST"
  exit 1
fi
