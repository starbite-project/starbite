#!/bin/sh
# Host test of kern/iris.c (the iris probe P1) against a fake phone (the Mac's cc). Run from anywhere; -v traces the log.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/sbos-iris-test.$$
cc -O1 -g -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -DIRIS_HOST -I"$here/.." \
    "$here/iris-test.c" "$here/../iris.c" -o "$out"
set +e
"$out" "$@"
rc=$?
rm -f "$out"
rm -rf "$out.dSYM"
exit $rc
