#!/bin/sh
# Host test of kern/sdclk.c (the microSD card's CMU divider) on a fake CMU (the Mac's cc). SDCLK_TEST_V=1 prints its log
# lines. Also checks that sdclk.c never calls a card writer or the card commands (only sd_set_bus_hook / sd_rebus).
set -e
here=$(cd "$(dirname "$0")" && pwd)
lib=$here/../../../libsb
out=${TMPDIR:-/tmp}/sbos-sdclk-test.$$
cc -c -O1 -std=gnu11 -Wall -Wextra -Werror -DSB_HOST -DSDCLK_HOST_FAKE -I"$lib" -I"$here/.." "$here/../sdclk.c" -o "$out.o"
bad=$(nm -u "$out.o" | grep -E 'sd_write|sb_write|sd_read|sd_up|sd_set_fast|sb_pmic|sb_acpm' || true)
rm -f "$out.o"
if [ -n "$bad" ]; then echo "sdclk-test: sdclk.c calls something it must not: $bad"; exit 1; fi
cc -O1 -g -std=gnu11 -Wall -Wextra -Werror -DSB_HOST -DSDCLK_HOST_FAKE -fsanitize=address,undefined -I"$lib" -I"$here/.." \
    "$here/sdclk-test.c" "$here/../sdclk.c" -o "$out"
rc=0
"$out" "$@" || rc=$?
rm -f "$out"
rm -rf "$out.dSYM"
exit $rc
