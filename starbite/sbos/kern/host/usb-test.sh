#!/bin/sh
# Host test of kern/usb.c and kern/usbacm.c against a fake MAX77705, fake registers and a fake DWC3 driver + host
# (the Mac's cc). Run from anywhere; -v traces the log.
set -e
here=$(cd "$(dirname "$0")" && pwd)
sb=$(cd "$here/../../.." && pwd)
out=${TMPDIR:-/tmp}/sbos-usb-test.$$
cc -O1 -g -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -DSB_HOST -DSBOS_KERNEL \
    -I"$sb/libsb" -I"$here/.." -I"$sb/sbos/sdk" -I"$sb/utility" -I"$sb/guest/sbos" \
    "$here/usb-test.c" "$here/../usb.c" "$here/../usbacm.c" "$sb/libsb/str.c" "$sb/utility/tb.c" -o "$out"
rc=0
"$out" "$@" || rc=$?
rm -f "$out"
rm -rf "$out.dSYM"
exit $rc
