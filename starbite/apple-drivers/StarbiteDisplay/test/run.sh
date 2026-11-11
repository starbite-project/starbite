#!/bin/sh
# Host tests for StarbiteDisplay's hw/ layer: clang on the Mac, a mock register file, no phone.
#   sh starbite/apple-drivers/StarbiteDisplay/test/run.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/starbite-display-test.$$
trap 'rm -rf "$out"' EXIT
mkdir -p "$out"
cc -std=c11 -O1 -g -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -o "$out/test_display" \
    "$here/test_display.c" "$here/mock_mmio.c" \
    "$here/../hw/sbd_decon.c" "$here/../hw/sbd_dsim.c" "$here/../hw/sbd_panel.c" "$here/../hw/sbd_swap.c"
"$out/test_display"
