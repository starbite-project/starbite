#!/bin/sh
# Host test of kern/shub_more.c + kern/senstat.c (-DSB_HOST) against a fake hub, survival and registers, with the real
# shub_proto.c frame parser (the Mac's cc). -v traces the log.
set -e
here=$(cd "$(dirname "$0")" && pwd)
sb=$here/../../..
out=${TMPDIR:-/tmp}/sbos-shub-more-test.$$
cc -O1 -g -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -DSB_HOST -DSBOS_KERNEL \
    -I"$sb/libsb" -I"$here/.." -I"$sb/sbos/sdk" -I"$sb/utility" -I"$sb/wifi" -I"$sb/guest/sbos" \
    "$here/shub-more-test.c" "$here/../shub_more.c" "$here/../senstat.c" "$here/../shub_proto.c" "$sb/libsb/str.c" \
    -o "$out" -lm
rc=0
"$out" "$@" || rc=$?
rm -f "$out"
rm -rf "$out.dSYM"
exit $rc
