#!/bin/sh
# libsb's half of the loader <-> OS interface (docs/STARBITE-FIRMWARE-LAYER.md section 10): the SBFW table, the
# settings record's SBFW part and the home store (the card's when the loader says so) on fake media, with the
# REAL libsb sbfw.c, store.c, fence.c, blkio.c and str.c (sbfw-store-test.c). Runs on the Mac (cc) or in the
# Lima VM (gcc):  sh starbite/host/sbfw-store-test.sh [-v]
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
lib="$root/starbite/libsb"
out=${SBOS_OUT:-"$root/starbite/out"}/sbfw-store-test
mkdir -p "$out"
CC=${CC:-cc}
san="-fsanitize=address,undefined -fno-sanitize-recover=all"
$CC -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -D_FORTIFY_SOURCE=0 -DSB_HOST -DSB_HOST_FAKE_MEM $san -I"$lib" \
    -o "$out/sbfw-store-test" "$root/starbite/host/sbfw-store-test.c" \
    "$lib/sbfw.c" "$lib/store.c" "$lib/fence.c" "$lib/blkio.c" "$lib/str.c"
"$out/sbfw-store-test" "$root/starbite/host/fixtures/sbfw" "$@"
