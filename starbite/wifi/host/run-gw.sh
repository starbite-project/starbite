#!/bin/sh
# sbgw's host test (gw.c, the Linux guest's gateway): freestanding build first, then host/gw-test.c under ASan/UBSan
# (a guest, two uplinks and a simulated Internet; 200k mutated packets at the end). The real-stack test is
# host/run-gw-netns.sh (in the VM, as root).
#   sh starbite/wifi/host/run-gw.sh [v]      v: the gateway's and sbnet's log lines
set -eu
here=$(cd "$(dirname "$0")" && pwd)
core="$here/.."
: "${CC:=cc}"
OUT=${OUT:-"$here/out"}
mkdir -p "$OUT"
for f in gw net; do
    $CC -std=c11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -nostdlib -c -I"$core" "$core/$f.c" -o "$OUT/$f-free.o"
done
$CC -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=address,undefined \
    -fno-sanitize-recover=undefined -I"$core" "$core/gw.c" "$core/net.c" "$core/tcp.c" "$here/gw-test.c" -o "$OUT/gw-test"
"$OUT/gw-test" ${1:-}
