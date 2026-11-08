#!/bin/bash
# Starbite OS 0.9 app support, host prototype (docs/STARBITE-OS-LINUX-M3.md 7): sbsim (Starbite OS's stand-in)
# on CPU 0 of a QEMU virt machine (GICv2, 8 CPUs) starts the S9's Linux 7.1 kernel on CPUs 4..7.
# Run in the Lima VM (native aarch64 gcc, qemu-system-aarch64, dtc, the kernel build tree):
#   limactl shell starbite -- /Users/dev/project-starbite/starbite/guest/run-qemu.sh SCENARIO
# Scenarios:
#   nomod      stock kernel, no sbguest.ko, no repair: shows both hazards (the GIC reset takes sbsim's line;
#              no clean way to power the cluster off) -- expected FAIL
#   mod        stock kernel + sbguest.ko: the cluster powers off cleanly; the GIC hazard stays -- FAIL
#   repair     stock kernel + sbguest.ko + sbsim re-asserts its lines after the guest's GIC init -- PASS
#   patched    IMAGE=<kernel with linux/0001-*.patch> + sbguest.ko, strict (no line may be lost) -- PASS
#   stub       no Linux: the phone probe's M3 stub (probe/m3stub.S, HVC build) on CPUs 4..7 -- PASS
#   kern       Starbite OS 1.2-alpha1's own guest service (sbos/guest.c, -DGUEST_QEMU) under sbsim/kern-shim.c:
#              the phone self-test image's guest_selftest_step (two cycles, the tap's pixel at 2x), strict -- PASS
# Environment: IMAGE (default: the build tree's Image), WORK (default /mnt/build/sbguest), RUNS (2),
# TIMEOUT (QEMU wall clock, 900 s), SURF (720x1440), PSCI01=1 (the guest gets the S9-style PSCI 0.1 node).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
scen=${1:-repair}
KSRC=${KSRC:-/mnt/build/src/mainline}
KOUT=${KOUT:-$KSRC/out}
IMAGE=${IMAGE:-$KOUT/arch/arm64/boot/Image}
WORK=${WORK:-/mnt/build/sbguest}
RUNS=${RUNS:-2}
TIMEOUT=${TIMEOUT:-900}
SURF=${SURF:-720x1440}
BUSYBOX=${BUSYBOX:-/mnt/build/out/busybox-arm64}
DB_GUEST=132        # SPI 100: host -> guest doorbell (QEMU virt leaves SPIs 64.. unused)
DB_HOST=${DB_HOST:-133}         # SPI 101: guest -> host doorbell
mkdir -p "$WORK"/{sbsim,mod,rd/bin,out}
cd "$WORK"

echo "== sbsim"
gcc -O2 -g0 -ffreestanding -nostdlib -nostartfiles -fno-pic -fno-pie -no-pie -static -mgeneral-regs-only \
    -mstrict-align -fno-stack-protector -fno-builtin -Wall -Wextra -Wno-unused-const-variable \
    -T "$here/sbsim/link.ld" "$here/sbsim/start.S" "$here/sbsim/sbsim.c" "$here/host/sbghost.c" -DM3STUB_HVC "$here/probe/m3stub.S" -o sbsim/sbsim.elf

if [ "$scen" = kern ]; then
    echo "== kern-shim (Starbite OS's guest.c, GUEST_QEMU)"
    sb="$here/../sbos"
    lib="$here/../libsb"
    gcc -O2 -g0 -ffreestanding -nostdlib -nostartfiles -fno-pic -fno-pie -no-pie -static -mgeneral-regs-only \
        -mstrict-align -fno-stack-protector -fno-builtin -Wall -Wextra -Werror -Wno-unused-const-variable \
        -DGUEST_QEMU -DSBOS_KERNEL -I"$lib" -I"$sb/kern" -I"$sb/sdk" -I"$here/sbos" \
        -T "$here/sbsim/link.ld" "$here/sbsim/start.S" "$here/sbsim/kern-shim.c" "$here/sbos/guest.c" \
        "$here/host/sbghost.c" "$lib/str.c" -o sbsim/kern-shim.elf
fi

echo "== guestd"
gcc -O2 -static -pthread -Wall -Wextra -Wno-unused-const-variable "$here/guestd/guestd.c" -o rd/guestd

echo "== sbguest.ko against $(cat "$KOUT/include/config/kernel.release")"
cp "$here/sbshm.h" mod/sbshm.h
mkdir -p mod/linux
cp "$here/linux/sbguest.c" "$here/linux/Makefile" mod/linux/
make -s -C "$KSRC" O="$KOUT" M="$WORK/mod/linux" modules
cp mod/linux/sbguest.ko rd/sbguest.ko

echo "== initramfs"
cp "$BUSYBOX" rd/bin/busybox
cp "$here/initramfs/init" rd/init
chmod +x rd/init rd/guestd rd/bin/busybox
mkdir -p rd/proc rd/sys rd/dev
find rd -exec touch -h -d @0 {} +       # reproducible: fixed mtimes, no gzip name / time
(cd rd && find . | sort | cpio -o -H newc --quiet --owner=0:0 --reproducible | gzip -9 -n) > out/initrd.cpio.gz

if [ "${BUILD_ONLY:-0}" = 1 ]; then    # probe/build.sh: guestd, sbguest.ko and the initramfs only
    echo "== built: $WORK/out/initrd.cpio.gz"
    exit 0
fi
extra=""
flags=0
case "$scen" in
    nomod) extra="sbguest.nomod" ;;
    mod) ;;
    repair) flags=1 ;;
    patched) flags=2 ;;
    stub) flags=4 ;;
    kern) flags=2 ;;
    *) echo "unknown scenario $scen"; exit 2 ;;
esac
echo "== guest DTB ($scen)"
rdsize=$(stat -c %s out/initrd.cpio.gz)
python3 "$here/mk-guest-dts.py" --profile qemu --rd-size "$rdsize" --db-guest $DB_GUEST \
    --bootargs-extra "$extra" ${PSCI01:+--psci01} out/guest.dts
dtc -q -I dts -O dtb -o out/guest.dtb out/guest.dts

echo "== store ($IMAGE)"
python3 - "$IMAGE" out/guest.dtb out/initrd.cpio.gz out/store.bin "$flags" "$RUNS" "$DB_GUEST" "$DB_HOST" "$SURF" <<'EOF'
import struct, sys
img, dtb, rd, out, flags, runs, dbg, dbh, surf = sys.argv[1:]
w, h = (int(x) for x in surf.split('x'))
blobs = [open(p, 'rb').read() for p in (img, dtb, rd)]
off, offs = 0x1000, []
for b in blobs:
    offs.append(off)
    off = (off + len(b) + 0xfff) & ~0xfff
m = struct.pack('<QII6QIIIIIIII', 0x3145524f54534253, int(flags), int(runs),
                offs[0], len(blobs[0]), offs[1], len(blobs[1]), offs[2], len(blobs[2]),
                int(dbg), int(dbh), w, h, w // 2, h // 2 + 120, 300, 0)
data = bytearray(off)
data[:len(m)] = m
for o, b in zip(offs, blobs):
    data[o:o + len(b)] = b
open(out, 'wb').write(data)
print(f'store: {len(data)} bytes, Image {len(blobs[0])}, DTB {len(blobs[1])}, initrd {len(blobs[2])}')
EOF

echo "== QEMU ($scen)"
log=out/qemu-$scen.log
rm -f sbsim-panel-run*.ppm
set +e
timeout "$TIMEOUT" qemu-system-aarch64 -M virt,gic-version=2,virtualization=off,secure=off \
    -cpu cortex-a53 -smp 8 -m 2048 -nographic -no-reboot \
    -semihosting-config enable=on,target=native \
    -kernel "$([ "$scen" = kern ] && echo sbsim/kern-shim.elf || echo sbsim/sbsim.elf)" \
    -device loader,file=out/store.bin,addr=0x48000000,force-raw=on 2>&1 | tee "$log"
set -e
for p in sbsim-panel-run*.ppm; do [ -f "$p" ] && mv "$p" "out/${scen}-${p}"; done
echo "== $(grep -a -E '(sbsim|kern-shim): RESULT' "$log" || echo 'sbsim: no RESULT line (the guest took the machine down?)')"
