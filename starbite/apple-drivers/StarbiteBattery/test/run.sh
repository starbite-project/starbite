#!/bin/sh
# SPDX-License-Identifier: APSL-2.0
# StarbiteBattery host tests: hw/ against a register-level HSI2C + MAX77705 mock (test/test_battery.c).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/sbbat.XXXXXX")
trap 'rm -rf "$out"' EXIT
cc=${CC:-clang}
flags="-std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -O1 -g -fsanitize=address,undefined"
$cc $flags -o "$out/test_battery" "$here/test_battery.c" "$here/../hw/exynos_hsi2c.c" "$here/../hw/max77705.c"
"$out/test_battery" "$@"
