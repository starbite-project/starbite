#!/bin/sh
# Host test of kern/cam.c and kern/cam_isp.c against a fake phone (the Mac's cc). Run from anywhere; -v traces the log.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/sbos-cam-test.$$
cc -O1 -g -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$here/.." \
    "$here/cam-test.c" "$here/../cam.c" "$here/../cam_isp.c" -o "$out"
set +e
"$out" "$@"
rc=$?
rm -f "$out"
rm -rf "$out.dSYM"
exit $rc
