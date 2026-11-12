#!/bin/sh
# Host test of kern/photos.c (Track A: the Camera's photo store on the volatile entry "sbos-photos") with libsb's real
# str.c, with the Mac's cc (or gcc in the VM), ASan and UBSan. Run from anywhere; -v lists every check.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
lib="$root/starbite/libsb"
out=${TMPDIR:-/tmp}/sbos-photos-test.$$
${CC:-cc} -O1 -g -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -D_FORTIFY_SOURCE=0 -DSB_HOST -DSB_HOST_FAKE_MEM \
    -DSBOS_KERNEL -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$here/.." -I"$lib" -I"$root/starbite/sbos/sdk" -I"$root/starbite/guest/sbos" \
    "$here/photos-test.c" "$here/../photos.c" "$lib/str.c" -o "$out"
set +e
"$out" "$@"
rc=$?
rm -f "$out"
rm -rf "$out.dSYM"
exit $rc
