#!/bin/bash
# The terminal on QEMU (docs/STARBITE-OS-LINUX-M3.md 12, steps 2 + 3): the guest kernel of its own (Image-guest from
# linux/build-patched.sh GUEST=1: linux/guest.config, simpledrm + fbcon on the shared surface), sbguest.ko built
# against it, the S9-style PSCI 0.1 node, a shell on tty1 (sbguest.mode=term). sbsim (flags TERM | STRICT) types
# "echo STARBITE-TERM-OK > /dev/kmsg" + Enter as key events, waits for that line in the guest's kernel log, checks
# the panel against the guest's frame row by row and that it shows text, then stops the guest; RUNS times.
#   limactl shell starbite -- /Users/dev/project-starbite/starbite/guest/run-term.sh
# Environment: WORK (/mnt/build/sbguest), IMAGE ($WORK/Image-guest), KOUT ($WORK/gout), RUNS (2), TIMEOUT (900 s),
# SURF (720x1440), EXTRA (guest bootargs), MODE (term | wayland).
# guestd, sbguest.ko and the initramfs come from run-qemu.sh BUILD_ONLY=1 (one recipe).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-/mnt/build/sbguest}
IMAGE=${IMAGE:-$WORK/Image-guest}
KOUT=${KOUT:-$WORK/gout}
RUNS=${RUNS:-2}
TIMEOUT=${TIMEOUT:-900}
SURF=${SURF:-720x1440}                    # the terminal surface (starbite-os 1.2: 720x1060 with fbcon=font:TER16x32)
EXTRA=${EXTRA:-}                          # more guest bootargs, e.g. "fbcon=font:TER16x32"
DB_GUEST=132
DB_HOST=133
[ -f "$IMAGE" ] || { echo "run-term: no $IMAGE (linux/build-patched.sh GUEST=1)"; exit 1; }
MODE=${MODE:-term}                        # term (fbcon + a shell) or wayland (sbwl + foot: docs/STARBITE-LINUX-APPS.md)
if [ "$MODE" = wayland ]; then
    "$here/sbwl/mk-initrd.sh" > "$WORK/out/run-term-build.log" 2>&1 || { tail -20 "$WORK/out/run-term-build.log"; exit 1; }
    RD=out/initrd-wl.cpio.gz
    DTSARGS=""
    EXTRA="sbguest.mode=wayland $EXTRA"
else
    BUILD_ONLY=1 WORK="$WORK" KOUT="$KOUT" "$here/run-qemu.sh" term > "$WORK/out/run-term-build.log" 2>&1 ||
        { tail -20 "$WORK/out/run-term-build.log"; exit 1; }
    RD=out/initrd.cpio.gz
    DTSARGS="--fb $SURF --term"
fi
cd "$WORK"
rdsize=$(stat -c %s $RD)
python3 "$here/mk-guest-dts.py" --profile qemu --psci01 $DTSARGS --rd-size "$rdsize" --db-guest $DB_GUEST \
    --bootargs-extra "$EXTRA" out/term.dts
dtc -q -I dts -O dtb -o out/term.dtb out/term.dts
python3 - "$IMAGE" out/term.dtb $RD out/store-term.bin 10 "$RUNS" "$DB_GUEST" "$DB_HOST" "$SURF" <<'EOF'
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
                int(dbg), int(dbh), w, h, w // 2, h // 2, 300, 0)
data = bytearray(off)
data[:len(m)] = m
for o, b in zip(offs, blobs):
    data[o:o + len(b)] = b
open(out, 'wb').write(data)
print(f'run-term: store {len(data)} bytes, Image {len(blobs[0])}, DTB {len(blobs[1])}, initrd {len(blobs[2])}')
EOF
log=out/qemu-term.log
rm -f sbsim-term-run*.ppm
set +e
timeout "$TIMEOUT" qemu-system-aarch64 -M virt,gic-version=2,virtualization=off,secure=off \
    -cpu cortex-a53 -smp 8 -m 2048 -nographic -no-reboot \
    -semihosting-config enable=on,target=native \
    -kernel sbsim/sbsim.elf \
    -device loader,file=out/store-term.bin,addr=0x48000000,force-raw=on > "$log" 2>&1
set -e
for p in sbsim-term-run*.ppm; do [ -f "$p" ] && mv "$p" "out/$p"; done
grep -a 'sbsim: \|STARBITE-TERM\|guest-init\|simpledrm\|fb0\|Console:\|sbwl:' "$log" | grep -v 'linux| .*echo' | cut -c1-200 | tail -40
echo "== $(grep -a 'sbsim: RESULT' "$log" || echo 'sbsim: no RESULT line')"
