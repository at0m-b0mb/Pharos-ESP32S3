#!/usr/bin/env bash
# Pharos - source wiring audit
#
# Why this exists.
#
# An engine lives in two builds at once: components/pharos_engine/CMakeLists.txt
# (the firmware) and test/host/Makefile (the laptop tests). It is easy - and has
# happened twice in this repo - to add a .c file to one and forget the other.
# The failure is silent and nasty in both directions:
#
#   * missing from the firmware  -> the feature is fully host-tested, ships
#     "green", and is simply ABSENT on the device. pharos_console.c did exactly
#     this: 300+ passing checks for a console the board did not contain.
#   * missing from the host build -> the engine ships untested.
#
# Neither shows up as a build error, so the only defence is to look. This runs
# in CI on every commit.
#
#   tools/check_sources.sh
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2

RED=$'\033[31m'; GRN=$'\033[32m'; RST=$'\033[0m'
fail=0
ok()  { printf '%s  ok%s   %s\n' "$GRN" "$RST" "$*"; }
bad() { printf '%s  FAIL%s %s\n' "$RED" "$RST" "$*"; fail=1; }

echo "Pharos source wiring audit"
echo "=========================="

echo
echo "[1] every pharos_engine source is in BOTH the firmware and host builds"
for f in components/pharos_engine/*.c; do
  b=$(basename "$f")
  in_idf=$(grep -c "\"${b}\"" components/pharos_engine/CMakeLists.txt || true)
  in_host=$(grep -c "pharos_engine/${b}" test/host/Makefile || true)
  if [ "$in_idf" -eq 0 ] && [ "$in_host" -eq 0 ]; then
    bad "$b is in NEITHER build"
  elif [ "$in_idf" -eq 0 ]; then
    bad "$b is host-tested but NOT compiled into the firmware"
  elif [ "$in_host" -eq 0 ]; then
    bad "$b is in the firmware but has NO host tests compiled"
  else
    ok "$b"
  fi
done

echo
echo "[2] every pharos_ui / pharos_core source is in the firmware build"
for comp in pharos_ui pharos_core pharos_radio pharos_bsp; do
  for f in components/$comp/*.c; do
    [ -e "$f" ] || continue
    b=$(basename "$f")
    if grep -q "\"${b}\"" "components/$comp/CMakeLists.txt"; then
      ok "$comp/$b"
    else
      bad "$comp/$b is not in components/$comp/CMakeLists.txt"
    fi
  done
done

echo
echo "[3] every main/*.c is in main/CMakeLists.txt"
for f in main/*.c; do
  b=$(basename "$f")
  if grep -q "\"${b}\"" main/CMakeLists.txt; then
    ok "main/$b"
  else
    bad "main/$b is not in main/CMakeLists.txt - it will not be built"
  fi
done

echo
echo "[4] every lens component is in main's REQUIRES"
for d in components/pharos_lens_*/; do
  name=$(basename "$d")
  if grep -q "\b${name}\b" main/CMakeLists.txt; then
    ok "$name"
  else
    bad "$name is not in main/CMakeLists.txt REQUIRES"
  fi
done

echo
if [ "$fail" -eq 0 ]; then
  printf '%sSOURCES WIRED%s - firmware and host builds agree.\n' "$GRN" "$RST"
  exit 0
else
  printf '%sSOURCE WIRING BROKEN%s - a feature is missing from a build.\n' "$RED" "$RST"
  exit 1
fi
