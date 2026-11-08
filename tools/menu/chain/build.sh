#!/bin/sh
# menuchain: sh tools/menu/chain/build.sh LOADER.Image OUT.img [--ramdisk RD]
# (stub built in the Lima VM; the Image packed on the Mac)
set -eu
root=$(cd "$(dirname "$0")/../../.." && pwd)
d="$root/tools/menu/chain"
tmp="$root/build/menu/chain-obj"
mkdir -p "$tmp"
CF='-c -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -nostdlib -fno-asynchronous-unwind-tables -mgeneral-regs-only -mstrict-align -mcpu=cortex-a53 -Wall -Wextra -Werror'
limactl shell starbite -- bash -lc "set -eu; cd '$d'
gcc $CF chain.S -o '$tmp/chain_S.o'; gcc $CF chain.c -o '$tmp/chain_c.o'; gcc $CF ../../../shim/fdt.c -o '$tmp/fdt.o'
ld -static -T image.ld --build-id=none --no-warn-rwx-segments '$tmp/chain_S.o' '$tmp/chain_c.o' '$tmp/fdt.o' -o '$tmp/chain.elf'
if readelf -r '$tmp'/*.o | grep -q ABS64; then echo 'absolute relocation' >&2; exit 1; fi
objcopy -O binary '$tmp/chain.elf' '$tmp/chain.bin'"
loader=$1 out=$2; shift 2
python3 "$d/pack.py" "$tmp/chain.bin" "$loader" "$out" "$@"
rm -rf "$tmp"
