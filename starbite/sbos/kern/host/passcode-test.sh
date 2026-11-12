#!/bin/sh
# Host test of kern/passcode.c (Track A): SHA-256, set / check / change / remove, the back-off on a fake clock, the
# record's round trip, no digits in the record. Run from anywhere (the Mac's cc or the VM's gcc); -v lists each check.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/sbos-passcode-test.$$
${CC:-cc} -O2 -g -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$here/.." \
    "$here/passcode-test.c" "$here/../passcode.c" -o "$out"
set +e
"$out" "$@"
rc=$?
rm -f "$out"
rm -rf "$out.dSYM"
exit $rc
