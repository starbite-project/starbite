#!/bin/sh
# Build SbWifi.sys (stage W1, docs/WIFI-BCM4361.md section 7) with the shared Windows kit (tools/win, on master
# since 2026-11-07; TOOLS_WIN= points at a checkout that has it) and sign it with the Starbite test certificate.
#   sh starbite/wifi/win/build.sh            -> starbite/out/win/sbwifi.sys
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
w="$root/starbite/wifi"
tw=${TOOLS_WIN:-}
for c in "$root/tools/win" "$root/../../../tools/win"; do
    if [ -z "$tw" ] && [ -x "$c/build.sh" ]; then tw=$c; fi
done
[ -n "$tw" ] || { echo "sbwifi: no tools/win (run on a checkout with it, or set TOOLS_WIN)" >&2; exit 1; }
out="$root/starbite/out/win"
mkdir -p "$out"
"$tw/build.sh" kmdf "$out/sbwifi.sys" "$here/sbwifi.c" "$w/core.c" "$w/exynos_pcie.c" "$w/chip.c" "$w/fw.c" "$w/ring.c" \
    "$w/msgbuf.c"
key=${SB_SIGN_KEY:-"$root/build/woa/ufs-sys/sign/starbite-test.key"}
[ -f "$key" ] || key="$root/../../../build/woa/ufs-sys/sign/starbite-test.key"
SB_SIGN_KEY=$key SB_SIGN_CERT=${SB_SIGN_CERT:-"$(dirname "$key")/starbite-test.crt"} "$tw/sign.sh" "$out/sbwifi.sys"
