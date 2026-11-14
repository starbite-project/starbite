#!/bin/sh
# Host test of kern/irisprobe.c's decoders, on the Mac's cc.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/sbos-irisprobe-test.$$
cc -O1 -g -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$here/.." "$here/irisprobe-test.c" -o "$out"
"$out"
rc=$?
rm -rf "$out" "$out.dSYM"
exit $rc
