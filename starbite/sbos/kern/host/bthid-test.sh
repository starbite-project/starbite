#!/bin/sh
# Host test of the Bluetooth keyboard stack (kern/bt_pair.c, bt_l2cap.c, bt_hid.c) against a scripted controller and a
# hand-written fake keyboard (the Mac's cc). Run from anywhere; -v traces the log and every key.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/sbos-bthid-test.$$
${CC:-cc} -O1 -g -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$here/.." \
    "$here/bthid-test.c" "$here/../bt_pair.c" "$here/../bt_l2cap.c" "$here/../bt_hid.c" "$here/../bt_hci.c" -o "$out"
rc=0
"$out" "$@" || rc=$?
rm -f "$out"
rm -rf "$out.dSYM"
exit $rc
