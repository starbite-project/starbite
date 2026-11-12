#!/bin/sh
# Host test of kern/bathealth.c (battery health, read only) against a fake fuel gauge (the Mac's cc). Run from
# anywhere; -v traces the log. First checks that bathealth.c references no writer at all (gauge, MAX77705, PMIC, I2C).
set -e
here=$(cd "$(dirname "$0")" && pwd)
lib=$here/../../../libsb
out=${TMPDIR:-/tmp}/sbos-bathealth-test.$$
cc -c -O1 -std=gnu11 -Wall -Wextra -Werror -DSB_HOST -I"$lib" -I"$here/.." "$here/../bathealth.c" -o "$out.o"
bad=$(nm -u "$out.o" | grep -E 'write|sb_i2c_msg|sb_gate|sb_auth|sb_pmic|sb_acpm|sb_chg|sb_usbc' || true)
rm -f "$out.o"
if [ -n "$bad" ]; then echo "bathealth-test: bathealth.c references a writer: $bad"; exit 1; fi
cc -O1 -g -std=gnu11 -Wall -Wextra -Werror -DSB_HOST -D_FORTIFY_SOURCE=0 -fsanitize=address,undefined -I"$lib" \
    -I"$here/.." "$here/bathealth-test.c" "$here/../bathealth.c" "$lib/str.c" -o "$out"
rc=0
"$out" "$@" || rc=$?
rm -f "$out"
rm -rf "$out.dSYM"
exit $rc
