#!/usr/bin/env bash
# Pharos - transmit fence audit (mechanism 4 of 4)
#
# Proves, by inspection of the source tree, that no transmit primitive is
# reachable except through the wrap traps in tx_fence.c. This is the cheap,
# fast check that runs on every commit; the link-time --wrap fence (mechanism
# 2) is what enforces it in the binary, and this script is what stops a
# transmit call being added in the first place.
#
# Exit 0 = fence intact. Non-zero = a transmit primitive appeared somewhere it
# should not, or a fence mechanism went missing. CI treats non-zero as a hard
# failure and blocks the merge.
#
#   tools/check_tx_fence.sh
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2

# The ELF path is captured HERE, before anything else runs, and every later
# reference uses this variable rather than "$1".
#
# It used to read "$1" at the point of use - roughly fifty lines after
#     set -- $BLE_VALS
# quietly reassigned the positional parameters to walk a list of expected
# Kconfig values. By the time the ELF stage was reached, "$1" was the string
# "y", `[ -f y ]` was false, and the entire linked-image audit was skipped.
# Silently: the guard has no else branch, so there was not even a "skipped"
# line to notice.
#
# That stage is the one that proves the --wrap traps actually made it into the
# firmware, and it is cited in the README and run by CI on every build. It has
# never once executed. The fence itself was intact the whole time - the wrap
# traps are present, as the stage now confirms - but the check that was
# supposed to prove it was answering a question about the letter "y".
ELF_PATH="${1:-}"

RED=$'\033[31m'; GRN=$'\033[32m'; YEL=$'\033[33m'; RST=$'\033[0m'
fail=0
note() { printf '  %s\n' "$*"; }
ok()   { printf '%s  ok%s   %s\n' "$GRN" "$RST" "$*"; }
bad()  { printf '%s  FAIL%s %s\n' "$RED" "$RST" "$*"; fail=1; }

echo "Pharos transmit-fence audit"
echo "==========================="

# The transmit primitives we refuse to let anything call. Kept in lockstep with
# the --wrap list in components/pharos_radio/CMakeLists.txt and the traps in
# tx_fence.c: three places, on purpose.
TX_SYMBOLS=(
  esp_wifi_80211_tx
  esp_wifi_deauth_sta
  esp_now_send
  esp_now_init
)

# 1. No transmit primitive may be *called* anywhere outside tx_fence.c.
echo
echo "[1] transmit primitives are not called outside the fence"
for sym in "${TX_SYMBOLS[@]}"; do
  # Match calls: "sym(" but not the wrap/real definitions or a comment mention.
  hits=$(grep -rnE "\b${sym}\s*\(" \
          --include='*.c' --include='*.cpp' --include='*.h' \
          components main 2>/dev/null \
        | grep -v 'components/pharos_radio/tx_fence.c' \
        | grep -vE '__(wrap|real)_' \
        | grep -vE '^\s*//|^\s*\*' )
  if [ -n "$hits" ]; then
    bad "$sym is called outside tx_fence.c:"
    echo "$hits" | sed 's/^/      /'
  else
    ok "$sym"
  fi
done

# 2. esp_wifi_set_mode must never request an AP mode.
echo
echo "[2] no access-point mode is ever requested"
apmode=$(grep -rnE 'esp_wifi_set_mode\s*\(\s*WIFI_MODE_(AP|APSTA)' \
          --include='*.c' components main 2>/dev/null \
        | grep -v 'components/pharos_radio/tx_fence.c')
if [ -n "$apmode" ]; then
  bad "an AP mode is requested:"
  echo "$apmode" | sed 's/^/      /'
else
  ok "only STA/NULL modes requested"
fi

# 3. The four fence mechanisms are present.
echo
echo "[3] fence mechanisms are in place"

cml="components/pharos_radio/CMakeLists.txt"
for sym in "${TX_SYMBOLS[@]}" esp_wifi_set_mode; do
  if grep -q -- "--wrap=${sym}" "$cml" 2>/dev/null; then
    ok "link wrap present for $sym"
  else
    bad "link wrap MISSING for $sym in $cml"
  fi
done

for sym in "${TX_SYMBOLS[@]}"; do
  if grep -qE "__wrap_${sym}\b" components/pharos_radio/tx_fence.c 2>/dev/null; then
    ok "trap present for $sym"
  else
    bad "trap MISSING for $sym in tx_fence.c"
  fi
done

# 4. NimBLE is configured observer-only.
echo
echo "[4] BLE is observer-only in the build config"
sdk="sdkconfig.defaults"
# Parallel arrays rather than an associative array, so this runs on the bash
# 3.2 that ships on macOS as well as the bash 5 in CI.
BLE_KEYS="CONFIG_BT_NIMBLE_ROLE_OBSERVER CONFIG_BT_NIMBLE_ROLE_BROADCASTER CONFIG_BT_NIMBLE_ROLE_CENTRAL CONFIG_BT_NIMBLE_ROLE_PERIPHERAL"
BLE_VALS="y n n n"
set -- $BLE_VALS
for key in $BLE_KEYS; do
  val="$1"; shift
  if grep -q "^${key}=${val}$" "$sdk" 2>/dev/null; then
    ok "${key}=${val}"
  else
    bad "${key} must be ${val} in $sdk"
  fi
done

# 5. There is no CAP_WIFI_TX / CAP_BLE_ADV token to grant.
echo
echo "[5] the BLE observer scans PASSIVELY"
# An ACTIVE BLE scan transmits: it answers every advertisement with a SCAN_REQ.
# That would be a frame emitted by this device, attributable to it, and it
# would break the one promise the project is built on. The scan parameters must
# therefore set passive=1, and no source may set it to 0.
if grep -q "p.passive = 1" components/pharos_radio/pharos_radio.c 2>/dev/null; then
  ok "BLE scan is passive (never sends SCAN_REQ)"
else
  bad "BLE scan parameters do not set passive=1 - an active scan TRANSMITS"
fi
if grep -rn "passive *= *0" components/ 2>/dev/null | grep -v "^Binary"; then
  bad "something sets passive=0 - that is an active, transmitting scan"
else
  ok "nothing requests an active scan"
fi

echo "[6] no transmit capability token exists"
# Match an actual #define of a transmit token, not the comment in caps.h that
# explains why no such token exists.
if grep -qE '^\s*#\s*define\s+PHAROS_CAP_(WIFI_TX|BLE_ADV|WIFI_INJECT|TX)\b' \
     components/pharos_core/include/pharos_caps.h 2>/dev/null; then
  bad "a transmit capability token is #defined - a lens could request transmit"
else
  ok "no transmit capability token defined"
fi

# 6. Optional: audit a linked ELF if one was passed in.
#
# Note on what this can and cannot prove. With --wrap, the linker keeps the
# ORIGINAL transmit symbol defined in the image (it is simply never called -
# every reference was redirected to __wrap_). So the presence of
# `esp_wifi_80211_tx` in the ELF is expected and not a breach. The meaningful
# ELF-level check is the inverse: that our __wrap_ traps are actually linked
# in, which proves the fence took effect at link time.
if [ -n "$ELF_PATH" ] && [ -f "$ELF_PATH" ]; then
  echo
  echo "[7] linked ELF has the wrap traps linked in: $ELF_PATH"
  if command -v nm >/dev/null 2>&1; then
    # Read the symbol table ONCE - see the long note in check_lenses.sh. Under
    # `set -o pipefail`, `nm "$ELF" | grep -q ...` reports a MISSING wrap trap
    # whenever grep matches early enough to SIGPIPE nm, which is a fence audit
    # failing on a fence that is intact.
    ELF_SYMS=$(nm "$ELF_PATH" 2>/dev/null)

    # WHAT A CLEAN IMAGE ACTUALLY LOOKS LIKE.
    #
    # The first version of this stage demanded a __wrap_<sym> trap for every
    # transmit primitive, and on a correct build four of the five are absent.
    # That is not a breach - it is a stronger result than the one being asked
    # for, and reading it as a failure had the audit condemning the very thing
    # it was written to protect.
    #
    # --wrap only rewrites references. If nothing in the firmware calls
    # esp_wifi_80211_tx, the linker never pulls in the archive member that
    # defines it, --gc-sections drops the unreferenced trap, and NEITHER symbol
    # appears. So an absent trap means "nothing can call this", which is better
    # than "calls to this are redirected".
    #
    # The breach to look for is therefore the specific combination: the
    # primitive is in the image AND its trap is not - a call that was not
    # redirected. Everything else is fine, and the two fine cases are reported
    # differently so the operator can see which one they got.
    wraps_seen=0
    for sym in "${TX_SYMBOLS[@]}"; do
      have_wrap=0; have_sym=0
      grep -qE "\b[Tt] __wrap_${sym}$" <<< "$ELF_SYMS" && have_wrap=1
      grep -qE "[[:space:]]${sym}$" <<< "$ELF_SYMS" && have_sym=1
      if [ "$have_wrap" -eq 1 ]; then
        wraps_seen=$((wraps_seen + 1))
        ok "${sym}: calls redirected to the trap"
      elif [ "$have_sym" -eq 0 ]; then
        ok "${sym}: not linked into the image at all"
      else
        bad "${sym} is in the image with NO trap - a call escaped the fence"
      fi
    done

    # THE POSITIVE CONTROL, and it is the load-bearing half of this stage.
    #
    # Every primitive above legitimately reports "not linked" on a clean build.
    # But so would every primitive if somebody deleted the --wrap flags from
    # the link line entirely - the traps would vanish and this stage would sail
    # through having proved nothing at all. An audit that cannot fail is not an
    # audit.
    #
    # So: at least one of the fence's own traps must be present in the image,
    # which can only happen if --wrap is genuinely being applied. tx_fence.c
    # also wraps esp_wifi_set_mode, and the radio HAL really does call it (to
    # ask for WIFI_MODE_NULL), so that trap IS linked on every honest build and
    # serves as the control. It is not in TX_SYMBOLS because stage [2] judges
    # AP-mode requests on its own terms.
    if ! grep -qE "\b[Tt] __wrap_esp_wifi_set_mode$" <<< "$ELF_SYMS"; then
      bad "__wrap_esp_wifi_set_mode is absent - the --wrap flags are not being applied, so the whole stage above proves nothing"
    else
      ok "wrap machinery is armed (the esp_wifi_set_mode trap is linked)"
    fi
  else
    note "nm not available; skipping ELF audit"
  fi
else
  # Say so out loud. A silent skip is what let this stage go missing.
  echo
  echo "[7] skipped (no ELF given; CI passes build/pharos.elf)"
fi

echo
if [ "$fail" -eq 0 ]; then
  printf '%sFENCE INTACT%s - receive-only posture verified.\n' "$GRN" "$RST"
  exit 0
else
  printf '%sFENCE BREACH%s - fix the above before this can ship.\n' "$RED" "$RST"
  exit 1
fi
