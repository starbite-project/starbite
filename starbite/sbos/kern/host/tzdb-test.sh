#!/bin/sh
# Host test of kern/tzdb.c (time zones) and ui/sh_timefmt.h (12 / 24-hour text). Run from anywhere (the Mac's cc or
# the VM's gcc); build.sh runs it on a native aarch64 host. tzdb-expect.inc comes from Python's zoneinfo.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/sbos-tzdb-test.$$
${CC:-cc} -O1 -g -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$here/.." -I"$here/../../../utility" \
    "$here/tzdb-test.c" "$here/../tzdb.c" "$here/../../../utility/tz.c" -o "$out"
rc=0
"$out" "$@" || rc=$?
rm -f "$out"
rm -rf "$out.dSYM"
exit $rc
