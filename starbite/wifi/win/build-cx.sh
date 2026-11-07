#!/bin/sh
# Build SbWifiCx.sys (stage W2, docs/WIFI-BCM4361.md section 7): the WiFiCx client driver (KMDF 1.33 + NetAdapterCx 2.1
# + WiFiCx 1.0, the versions Windows 11 22H2/23H2 ships) with the shared Windows kit (tools/win), signed with the
# Starbite test certificate. The INF (sbwificx.inf) is signed into a catalog next to it.
#   sh starbite/wifi/win/build-cx.sh         -> starbite/out/win/sbwificx.sys (+ sbwificx.inf, sbwificx.cat)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
w="$root/starbite/wifi"
tw=${TOOLS_WIN:-}
for c in "$root/tools/win" "$root/../../../tools/win"; do
    if [ -z "$tw" ] && [ -x "$c/build.sh" ]; then tw=$c; fi
done
[ -n "$tw" ] || { echo "sbwificx: no tools/win (run on a checkout with it, or set TOOLS_WIN)" >&2; exit 1; }
. "$tw/env.sh"
NETCX=${NETCX:-2.1} WIFICX=${WIFICX:-1.0}
i=$SBWIN_INC l=$SBWIN_LIB/km/arm64
out="$root/starbite/out/win"
mkdir -p "$out"
CFLAGS="${CFLAGS:-} -DNETADAPTER_VERSION_MAJOR=2 -DNETADAPTER_VERSION_MINOR=${NETCX#*.}
 -DNETADAPTER_MINIMUM_VERSION_REQUIRED=${NETCX#*.} -DWIFI_VERSION_MAJOR=1 -DWIFI_VERSION_MINOR=${WIFICX#*.}
 -DWIFI_MINIMUM_VERSION_REQUIRED=${WIFICX#*.}
 -imsvc $i/km/netcx/kmdf/adapter/$NETCX -imsvc $i/shared/netcx/shared/1.0 -imsvc $i/km/wificx/$WIFICX -imsvc $i/km/wlan/2.0" \
LIBS="${LIBS:-} cng.lib ndis.lib $l/netcx/kmdf/adapter/$NETCX/netadaptercxstub.lib $l/wificx/$WIFICX/wificxstub.lib
 $l/wlan/2.0/WificxTLVGenParse.lib libcntpr.lib" \
"$tw/build.sh" kmdf "$out/sbwificx.sys" "$here/sbwificx.cpp" "$w/core.c" "$w/exynos_pcie.c" "$w/chip.c" "$w/fw.c" "$w/ring.c" \
    "$w/msgbuf.c" "$w/link.c" "$w/wpa.c"
key=${SB_SIGN_KEY:-"$root/build/woa/ufs-sys/sign/starbite-test.key"}
[ -f "$key" ] || key="$root/../../../build/woa/ufs-sys/sign/starbite-test.key"
SB_SIGN_KEY=$key SB_SIGN_CERT=${SB_SIGN_CERT:-"$(dirname "$key")/starbite-test.crt"} "$tw/sign.sh" "$out/sbwificx.sys"
# the PnP package: INF + catalog (makecat.py, signed like the driver)
cp "$here/sbwificx.inf" "$out/"
python3 "$tw/makecat.py" "$out/sbwificx.cat" "$out/sbwificx.inf" "$out/sbwificx.sys" --hwid 'acpi\stbw4361'
SB_SIGN_KEY=$key SB_SIGN_CERT=${SB_SIGN_CERT:-"$(dirname "$key")/starbite-test.crt"} "$tw/sign.sh" "$out/sbwificx.cat"
