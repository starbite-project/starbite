#!/bin/sh
# Host test of libsb sd.c's fast reads (CMD6 high speed + IDMAC) on a fake controller and card (the Mac's cc).
# SD_TEST_V=1 prints the driver's log lines.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/sbos-sd-test.$$
cc -O1 -g -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -DSB_HOST -DSD_HOST_FAKE \
    -DSD_DMA_LIMIT=0xffffffffffffffffULL -I"$here/.." -I"$here/../../../libsb" "$here/sd-test.c" -o "$out"
set +e
"$out" "$@"
rc=$?
rm -f "$out"
rm -rf "$out.dSYM"
exit $rc
