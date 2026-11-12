#!/bin/sh
# Host test of kern/nledpol.c (Track A: the notification LED's policy) against a fake LED, with the Mac's cc (or gcc
# in the VM), ASan and UBSan. Run from anywhere; -v lists every check.
set -e
here=$(cd "$(dirname "$0")" && pwd)
lib=$here/../../../libsb
out=${TMPDIR:-/tmp}/sbos-nledpol-test.$$
${CC:-cc} -O1 -g -std=gnu11 -Wall -Wextra -Werror -DSB_HOST -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$lib" -I"$here/.." "$here/nledpol-test.c" "$here/../nledpol.c" -o "$out"
set +e
"$out" "$@"
rc=$?
rm -f "$out"
rm -rf "$out.dSYM"
exit $rc
