#!/bin/sh
# Host test of kern/update.c (Track A: Software Update) with libsb's real sbfw_api.c / sbfw.c / str.c and the real
# SHA-256 (kern/passcode.c), with the Mac's cc (or gcc in the VM), ASan and UBSan. Run from anywhere; -v lists every
# check.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
lib="$root/starbite/libsb"
out=${TMPDIR:-/tmp}/sbos-update-test.$$
${CC:-cc} -O1 -g -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -D_FORTIFY_SOURCE=0 -DSB_HOST -DSB_HOST_FAKE_MEM \
    -DSBOS_KERNEL -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$here/.." -I"$lib" -I"$root/starbite/sbos/sdk" -I"$root/starbite/guest/sbos" \
    "$here/update-test.c" "$here/../update.c" "$here/../passcode.c" "$lib/sbfw.c" "$lib/sbfw_api.c" "$lib/str.c" -o "$out"
set +e
"$out" "$@"
rc=$?
rm -f "$out"
rm -rf "$out.dSYM"
exit $rc
