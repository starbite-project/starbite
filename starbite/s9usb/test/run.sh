#!/usr/bin/env bash
# Host test of starbite/s9usb (no hardware): builds with the Mac's cc, -Wall -Wextra -Werror,
# also as a freestanding aarch64 object (the firmware's view), and runs the register checks.
set -euo pipefail
cd "$(dirname "$0")/.."
out=${TMPDIR:-/tmp}/s9usb-test.$$
mkdir -p "$out"
trap 'rm -rf "$out"' EXIT
cc -std=c11 -Wall -Wextra -Werror -O2 -o "$out/t" test/s9usb-host-test.c s9usb_phy.c s9usb_dwc3.c s9usb_max77705.c
for f in s9usb_phy.c s9usb_dwc3.c s9usb_max77705.c; do
    clang -target aarch64-none-elf -ffreestanding -fno-builtin -std=c11 -Wall -Wextra -Werror -O2 -c "$f" -o "$out/${f%.c}.o"
done
"$out/t"
