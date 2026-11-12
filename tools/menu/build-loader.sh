#!/bin/sh
# Build the starlte loader Image into its own directory (shim/starlte/build.sh's file list and flags,
# without touching shim/starlte/out), optionally with extra defines for the v7 files:
#
#   sh tools/menu/build-loader.sh <outdir> [-DM7_BENCH ...]
#
# Runs in the Lima VM (native aarch64 gcc). The result is <outdir>/xnu_boot.Image; the objects are
# deleted afterwards (disk is tight).
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd); shift
extra="$*"
limactl shell starbite -- bash -lc "set -eu; cd '$root/shim/starlte'
O='$out'
CFLAGS='-c -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -nostdlib -fno-asynchronous-unwind-tables -fno-jump-tables -fno-tree-switch-conversion -fno-tree-loop-distribute-patterns -mgeneral-regs-only -mstrict-align -mcpu=cortex-a53 -Wall -Wextra -Werror $extra'
CFLAGS_V7='-c -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -nostdlib -fno-asynchronous-unwind-tables -fno-jump-tables -fno-tree-switch-conversion -fno-tree-loop-distribute-patterns -mstrict-align -mcpu=cortex-a55 -fno-math-errno -ffp-contract=fast -Wall -Wextra -Werror $extra'
V7=\"\$(ls glass*.c) menu7.c intro7.c sb7.c\"
USB='usb_dwc3.c usb_phy.c usb_desc.c max77705_muic.c fastboot.c fastboot_dev.c'
objs=''
for src in xnu_boot.S xnu_boot.c mmu.c anim.c menu.c menu6.c ui.c em.c em_vec.S batt.c ufs.c ufs_fence.c store.c sd.c catalog.c fbtext.c ../fdt.c recovery.c post.c sbfmt.c dtmerge.c crash.c crashdoc.c wdt.c sbfw.c sbfwfs.c repair.c \$USB \$V7; do
    obj=\"\$O/\$(basename \$src | tr . _).o\"
    case \"\$(basename \$src)\" in
    glass*.c|menu7.c|intro7.c|sb7.c) gcc \$CFLAGS_V7 \$src -o \$obj ;;
    *) gcc \$CFLAGS \$src -o \$obj ;;
    esac
    objs=\"\$objs \$obj\"
done
ld -static -T image.ld --build-id=none --no-warn-rwx-segments \$objs -o \$O/xnu_boot.elf
if readelf -r \$objs | grep -q ABS64; then echo 'absolute relocation found' >&2; exit 1; fi
objcopy -O binary \$O/xnu_boot.elf \$O/xnu_boot.Image
nm \$O/xnu_boot.elf | awk '\$3 == \"_image_end\" { print \$1 }' > \$O/image_end.txt
nm -S --size-sort \$O/xnu_boot.elf | tail -5 > \$O/biggest.txt
rm -f \$objs
ls -l \$O/xnu_boot.Image
"
