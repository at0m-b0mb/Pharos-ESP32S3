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
echo "[5] no transmit capability token exists"
# Match an actual #define of a transmit token, not the comment in caps.h that
# explains why no such token exists.
if grep -qE '^\s*#\s*define\s+PHAROS_CAP_(WIFI_TX|BLE_ADV|WIFI_INJECT|TX)\b' \
     components/pharos_core/include/pharos_caps.h 2>/dev/null; then
  bad "a transmit capability token is #defined - a lens could request transmit"
else
  ok "no transmit capability token defined"
fi

# 6. Optional: audit a linked ELF if one was passed in.
if [ "${1:-}" != "" ] && [ -f "${1:-}" ]; then
  echo
  echo "[6] linked ELF carries no unwrapped transmit symbol: $1"
  if command -v nm >/dev/null 2>&1; then
    for sym in "${TX_SYMBOLS[@]}"; do
      # A defined (T/t) real symbol that is not the __real_ alias is suspicious.
      if nm "$1" 2>/dev/null | grep -qE "\bT ${sym}$"; then
        bad "$sym is defined in the ELF outside the wrap"
      else
        ok "$sym not defined unwrapped"
      fi
    done
  else
    note "nm not available; skipping ELF audit"
  fi
fi

echo
if [ "$fail" -eq 0 ]; then
  printf '%sFENCE INTACT%s - receive-only posture verified.\n' "$GRN" "$RST"
  exit 0
else
  printf '%sFENCE BREACH%s - fix the above before this can ship.\n' "$RED" "$RST"
  exit 1
fi
