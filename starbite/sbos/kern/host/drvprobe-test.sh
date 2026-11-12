#!/bin/sh
# Host test of kern/drvprobe.c (the driver probe runner) with fake probes, on the Mac's cc.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/sbos-drvprobe-test.$$
cc -O1 -g -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -DSB_HOST -I"$here/.." -I"$here/../../../libsb" \
    "$here/drvprobe-test.c" -o "$out"
"$out"
rc=$?
rm -rf "$out" "$out.dSYM"
exit $rc
