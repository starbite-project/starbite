#!/bin/sh
# smptest: build glass_smp.c's phone code for QEMU virt and run it on 4 cores (Lima VM).
#   sh tools/menu/smptest/run.sh
set -eu
root=$(cd "$(dirname "$0")/../../.." && pwd)
d="$root/tools/menu/smptest"
o="$root/build/menu/smptest"
mkdir -p "$o"
st="$root/shim/starlte"
CF="-c -O2 -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -fno-asynchronous-unwind-tables -mstrict-align -mcpu=cortex-a55 -Wall -Wextra -Werror -DG7_SMP_QEMU -I$st -I$root/shim"
limactl shell starbite -- bash -lc "set -eu; cd '$o'
gcc $CF '$d/start.S' -o start.o; gcc $CF '$d/test.c' -o test.o; gcc $CF '$st/glass_smp.c' -o smp.o
gcc $CF -mgeneral-regs-only '$st/mmu.c' -o mmu.o
ld -static -T '$d/image.ld' --build-id=none --no-warn-rwx-segments start.o test.o smp.o mmu.o -o smptest.elf
objcopy -O binary smptest.elf smptest.bin
timeout 120 qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a55 -smp 4 -accel tcg,thread=multi -m 4G -nographic \
  -device loader,file=smptest.bin,addr=0x80080000,force-raw=on -device loader,addr=0x80080000,cpu-num=0 2>&1 | head -60 &
sleep 100; pkill -f smptest.bin || true; wait"
