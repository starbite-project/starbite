#!/bin/sh
# Host test of kern/nled.c (the MAX77705 RGB LED) against a fake bus and gate (the Mac's cc). Run from anywhere; -v
# traces the log. Also checks that nled.c reaches the chip only through sb_i2c_read / sb_max_write (no raw I2C message,
# no gauge, PMIC or other write).
set -e
here=$(cd "$(dirname "$0")" && pwd)
lib=$here/../../../libsb
out=${TMPDIR:-/tmp}/sbos-nled-test.$$
cc -c -O1 -std=gnu11 -Wall -Wextra -Werror -DSB_HOST -I"$lib" -I"$here/.." "$here/../nled.c" -o "$out.o"
bad=$(nm -u "$out.o" | grep -E 'sb_i2c_msg|sb_fg_write16|sb_pmic_write|sb_acpm|sb_write|sb_chg|sb_usbc' || true)
rm -f "$out.o"
if [ -n "$bad" ]; then echo "nled-test: nled.c calls a writer it must not: $bad"; exit 1; fi
cc -O1 -g -std=gnu11 -Wall -Wextra -Werror -DSB_HOST -D_FORTIFY_SOURCE=0 -fsanitize=address,undefined -I"$lib" -I"$here/.." \
    "$here/nled-test.c" "$here/../nled.c" "$lib/str.c" -o "$out"
rc=0
"$out" "$@" || rc=$?
rm -f "$out"
rm -rf "$out.dSYM"
exit $rc
