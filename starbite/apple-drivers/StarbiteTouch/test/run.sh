#!/bin/sh
# SPDX-License-Identifier: APSL-2.0
# StarbiteTouch host tests: sh starbite/apple-drivers/StarbiteTouch/test/run.sh
set -eu
d=$(cd "$(dirname "$0")/.." && pwd)
out=${TMPDIR:-/tmp}/sbtouch-test.$$
trap 'rm -f "$out"' EXIT
clang -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I "$d/hw" -I "$d/test" \
    "$d/hw/sb_hsi2c.c" "$d/hw/sb_s6sy761.c" "$d/hw/sb_touch_gesture.c" "$d/hw/sb_touch_script.c" \
    "$d/test/mock.c" "$d/test/test_touch.c" -o "$out"
"$out"
