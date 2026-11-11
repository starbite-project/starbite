#!/bin/sh
# StarbiteUSB host tests: clang on the Mac, a mock controller, AddressSanitizer + UBSan. No phone.
#   sh starbite/apple-drivers/StarbiteUSB/test/run.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/sbusb-test.$$
mkdir -p "$out"
trap 'rm -rf "$out"' EXIT
hw="$here/../hw/sbusb_desc.c $here/../hw/sbusb_ncm.c $here/../hw/sbusb_ep0.c $here/../hw/sbusb_phy.c \
    $here/../hw/sbusb_dwc3.c $here/../hw/sbusb_func.c $here/../hw/sbusb_echo.c"
fail=0
for t in t_desc t_ncm t_ep0 t_ring; do
    # shellcheck disable=SC2086
    clang -std=c99 -O1 -g -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion \
        -fsanitize=address,undefined -fno-sanitize-recover=undefined \
        -o "$out/$t" "$here/$t.c" "$here/mock.c" $hw
    "$out/$t" || fail=1
done
[ "$fail" = 0 ] && echo "StarbiteUSB host tests: PASS" || { echo "StarbiteUSB host tests: FAIL"; exit 1; }
