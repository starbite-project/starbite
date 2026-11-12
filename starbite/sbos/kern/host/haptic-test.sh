#!/bin/sh
# Host test of kern/haptic.c on kern/vib.c (-DSB_HOST) against a simulated clock and timer (the Mac's cc). -v traces.
set -e
here=$(cd "$(dirname "$0")" && pwd)
sb=$here/../../..
out=${TMPDIR:-/tmp}/sbos-haptic-test.$$
cc -O1 -g -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -DSB_HOST -DSBOS_KERNEL \
    -I"$sb/libsb" -I"$here/.." -I"$sb/sbos/sdk" -I"$sb/utility" -I"$sb/wifi" -I"$sb/guest/sbos" \
    "$here/haptic-test.c" "$here/../haptic.c" "$here/../vib.c" "$sb/libsb/str.c" -o "$out"
rc=0
"$out" "$@" || rc=$?
rm -f "$out"
rm -rf "$out.dSYM"
exit $rc
