#!/bin/sh
# Host test of kern/bt.c, bt_uart.c, bt_hci.c and pcal6524.c against a fake UART, expander and Bluetooth
# controller (the Mac's cc). Run from anywhere; -v traces the log.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/sbos-bt-test.$$
cc -O1 -g -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$here/.." \
    "$here/bt-test.c" "$here/../bt.c" "$here/../bt_uart.c" "$here/../bt_hci.c" "$here/../pcal6524.c" -o "$out"
rc=0
"$out" "$@" || rc=$?
rm -f "$out"
rm -rf "$out.dSYM"
exit $rc
