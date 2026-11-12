#!/bin/sh
# Host harness of kern/wifi.c (wifi-kern-test.c): the kernel's Wi-Fi service on the real core (starbite/wifi), its
# TCP/HTTPS (BearSSL as vendored) and the chip / access point model (starbite/wifi/host/model*.c), one scenario per
# process (a process is a boot). On the Mac: sh starbite/sbos/kern/host/wifi-kern-test.sh [-v] [scenario...]
#   WIFI_C=another wifi.c (a bisect), CC=gcc (default cc), OUT=dir (default $TMPDIR/sbos-wifi-kern-test: BearSSL's objects are kept there)
set -u
here=$(cd "$(dirname "$0")" && pwd)
kern="$here/.."
s="$(cd "$here/../../.." && pwd)"
wifi="$s/wifi"
br="$wifi/tls/bearssl"
: "${CC:=cc}"
OUT=${OUT:-"${TMPDIR:-/tmp}/sbos-wifi-kern-test"}
mkdir -p "$OUT"
v=""
[ "${1:-}" = "-v" ] && { v=-v; shift; }
san="-fsanitize=address,undefined -fno-sanitize-recover=all"
brflags="-std=gnu11 -O1 -g -w $san -I$br/inc -I$br/src -DBR_USE_URANDOM=0 -DBR_USE_UNIX_TIME=0 -DBR_USE_GETENTROPY=0 \
-DBR_RDRAND=0 -DBR_LE_UNALIGNED=0 -DBR_BE_UNALIGNED=0"
brout="$OUT/br-$(basename "$CC")"
mkdir -p "$brout"
brobjs=""
for f in $(cat "$br/FILES"); do
    o="$brout/$(echo "$f" | tr / _ | sed 's/\.c$/.o/')"
    [ "$o" -nt "$br/src/$f" ] || $CC $brflags -c "$br/src/$f" -o "$o" || exit 1
    brobjs="$brobjs $o"
done
flags="-std=gnu11 -O1 -g -Wall -Wextra -Werror $san -DSB_HOST -DSBOS_KERNEL -I$s/libsb -I$kern -I$s/sbos/sdk -I$wifi \
-I$br/inc -I$s/guest/sbos -I$s/utility -I$here"
src="${WIFI_C:-$kern/wifi.c} $kern/state.c $s/libsb/str.c $here/wifi-kern-stubs.c $here/wifi-kern-test.c"
core="$wifi/core.c $wifi/exynos_pcie.c $wifi/chip.c $wifi/fw.c $wifi/ring.c $wifi/msgbuf.c $wifi/link.c $wifi/wpa.c \
$wifi/sae.c $wifi/net.c $wifi/tcp.c $wifi/netio.c $wifi/http.c $wifi/tls/ta.c $wifi/tls/tls_chains.c $wifi/tls/tls_check.c"
# the core and the model as their own tests build them (their warnings settings), the kernel's side with ours
cflags="-std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter $san -I$wifi -I$br/inc"
objs=""
for f in $core $wifi/host/model.c $wifi/host/model_ap.c; do
    o="$OUT/c_$(basename "$f" .c).o"
    $CC $cflags -c "$f" -o "$o" || exit 1
    objs="$objs $o"
done
bin="$OUT/wifi-kern-test-$(basename "$CC")"
# shellcheck disable=SC2086
$CC $flags -o "$bin" $src $objs $brobjs || exit 1
[ $# -gt 0 ] || set -- main persist hidden wpa3 open time time-survival selftest0 selftest1 h1-saved h1-nosaved h1-24
fails=0
total=0
for sc in "$@"; do
    if [ "$sc" = persist ]; then
        f="$OUT/wifi-known.bin"
        rm -f "$f"
        out=$("$bin" $v persist-write "$f" 2>&1; "$bin" $v persist-read "$f" 2>&1)
    else
        out=$("$bin" $v "$sc" 2>&1)
    fi
    echo "$out" | grep -v '^ok: ' | grep -v '^  \[' | grep -v '^wifi-kern-test ' || true
    [ -n "$v" ] && echo "$out" | grep '^  \['
    echo "$out" | grep '^wifi-kern-test '
    n=$(echo "$out" | grep -c '^FAIL: ')
    c=$(echo "$out" | grep -c '^\(ok\|FAIL\): ')
    if ! echo "$out" | grep -q '^wifi-kern-test '; then
        echo "FAIL: $sc: the process did not finish (a sanitizer or a crash; above)"
        n=$((n + 1))
    fi
    fails=$((fails + n))
    total=$((total + c))
done
echo "wifi-kern-test: $total checks, $fails failed ($CC)"
[ "$fails" -eq 0 ]
