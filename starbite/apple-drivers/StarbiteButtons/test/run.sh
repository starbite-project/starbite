#!/bin/sh
# SPDX-License-Identifier: APSL-2.0
# StarbiteButtons host tests: sh starbite/apple-drivers/StarbiteButtons/test/run.sh
set -eu
d=$(cd "$(dirname "$0")/.." && pwd)
out=${TMPDIR:-/tmp}/sbkeys-test.$$
trap 'rm -f "$out"' EXIT
clang -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I "$d/hw" "$d/hw/sb_keys.c" "$d/test/test_keys.c" -o "$out"
"$out"
