#!/bin/sh
# Host tests of StarbiteGICv2's register logic (hw/) against the GIC-400 model. Run: sh test/run.sh
set -eu
cd "$(dirname "$0")"
out=${TMPDIR:-/tmp}/sb-gicv2-test.$$
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
    -o "$out" test_gicv2.c mock_gic.c ../hw/sb_gicv2.c
"$out"
rm -f "$out"
