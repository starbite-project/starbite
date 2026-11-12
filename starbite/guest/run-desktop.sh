#!/bin/bash
# The desktop guest on QEMU (docs/STARBITE-LINUX-APPS.md L4): the guest kernel of its own (Image-guest built with
# linux/guest-desktop.config), sbguest.ko with sbblk, and a root on block devices that sbsim serves from files on
# this VM through semihosting: BASE (EROFS, read-only, /dev/sbblk0), OVERLAY (ext4, read-write, /dev/sbblk1; its
# file keeps what the guest wrote, so it persists across runs), REPO (EROFS, read-only, /dev/sbblk2, mounted at
# /repo). initramfs/init (sbguest.mode=desktop) mounts them and switches root to /sbin/sbinit of the base image.
#   limactl shell starbite -- env BASE=... OVERLAY=... /Users/dev/project-starbite/starbite/guest/run-desktop.sh
# Environment:
#   BASE      the base image file (required)          OVERLAY, REPO   optional files ("" = the device is absent)
#   WORK      /mnt/build/sbguest2/blk (sbsim, the initramfs, out/ live here)
#   IMAGE     /mnt/build/sbguest2/Image-guest          KOUT   /mnt/build/sbguest2/gout
#   FLAGS     sbsim's flags, default 66 = BLKTEST | STRICT: no scripted input, the run ends when the guest logs
#             SBBLK-TEST-DONE (blk/test-sbinit does), FAIL on a "sbblk-test: FAIL" line. A root that brings sbwl
#             (HELLO, frames) takes the flags run-term.sh documents instead.
#   RUNS      guest boots in one QEMU (1)              TIMEOUT   QEMU wall clock (1800 s)
#   BOOT_TIMEOUT  seconds sbsim waits for HELLO (3000: the block-test root never says HELLO)
#   EXTRA     more guest bootargs                      SURF   720x1440
#   DB_GUEST / DB_HOST   the doorbell INTIDs (132 / 133); DB_GUEST=0 DB_HOST=0: no doorbells, both sides poll
#   INITRD    an initramfs to use instead of the one built here (run-qemu.sh BUILD_ONLY=1)
#   LOG       out/qemu-desktop.log
#   SBSIM_CFLAGS  more flags for sbsim (with INITRD), e.g. -DSIM_P9_WRITABLE=0 (the read-only tree, as the phone)
# run-term.sh (MODE=term | wayland) is unchanged; this script is the block-device mode only.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-/mnt/build/sbguest2/blk}
IMAGE=${IMAGE:-/mnt/build/sbguest2/Image-guest}
KOUT=${KOUT:-/mnt/build/sbguest2/gout}
RUNS=${RUNS:-1}
TIMEOUT=${TIMEOUT:-1800}
BOOT_TIMEOUT=${BOOT_TIMEOUT:-3000}
SURF=${SURF:-720x1440}
EXTRA=${EXTRA:-}
FLAGS=${FLAGS:-66}
DB_GUEST=${DB_GUEST:-132}
DB_HOST=${DB_HOST:-133}
BASE=${BASE:-}
OVERLAY=${OVERLAY:-}
REPO=${REPO:-}
[ -f "$IMAGE" ] || { echo "run-desktop: no $IMAGE (linux/build-patched.sh GUEST=1 GCONF=linux/guest-desktop.config)"; exit 1; }
[ -n "$BASE" ] && [ -f "$BASE" ] || { echo "run-desktop: BASE=<the base EROFS image> is required"; exit 1; }
for f in "$OVERLAY" "$REPO"; do
    [ -z "$f" ] || [ -f "$f" ] || { echo "run-desktop: no $f"; exit 1; }
done
mkdir -p "$WORK/out"
if [ -n "${INITRD:-}" ]; then
    # sbsim alone (the recipe of run-qemu.sh)
    mkdir -p "$WORK/sbsim"
    gcc -O2 -g0 -ffreestanding -nostdlib -nostartfiles -fno-pic -fno-pie -no-pie -static -mgeneral-regs-only \
        -mstrict-align -fno-stack-protector -fno-builtin -Wall -Wextra -Wno-unused-const-variable \
        -T "$here/sbsim/link.ld" "$here/sbsim/start.S" "$here/sbsim/sbsim.c" "$here/host/sbghost.c" -DM3STUB_HVC \
        "$here/probe/m3stub.S" ${SBSIM_CFLAGS:-} -o "$WORK/sbsim/sbsim.elf"
    RD=$INITRD
else
    BUILD_ONLY=1 WORK="$WORK" KOUT="$KOUT" "$here/run-qemu.sh" desktop > "$WORK/out/run-desktop-build.log" 2>&1 ||
        { tail -30 "$WORK/out/run-desktop-build.log"; exit 1; }
    RD=$WORK/out/initrd.cpio.gz
fi
cd "$WORK"
LOG=${LOG:-out/qemu-desktop.log}
rdsize=$(stat -c %s "$RD")
python3 "$here/mk-guest-dts.py" --profile qemu --psci01 --rd-size "$rdsize" --db-guest "$DB_GUEST" \
    --bootargs-extra "sbguest.mode=desktop $EXTRA" out/desktop.dts
dtc -q -I dts -O dtb -o out/desktop.dtb out/desktop.dts
python3 - "$IMAGE" out/desktop.dtb "$RD" out/store-desktop.bin "$FLAGS" "$RUNS" "$DB_GUEST" "$DB_HOST" "$SURF" \
    "$BOOT_TIMEOUT" "$BASE" "$OVERLAY" "$REPO" <<'EOF'
import os, struct, sys
img, dtb, rd, out, flags, runs, dbg, dbh, surf, boot_to, base, overlay, repo = sys.argv[1:]
w, h = (int(x) for x in surf.split('x'))
blobs = [open(p, 'rb').read() for p in (img, dtb, rd)]
off, offs = 0x1000, []
for b in blobs:
    offs.append(off)
    off = (off + len(b) + 0xfff) & ~0xfff
m = struct.pack('<QII6QIIIIIIII', 0x3145524f54534253, int(flags), int(runs),
                offs[0], len(blobs[0]), offs[1], len(blobs[1]), offs[2], len(blobs[2]),
                int(dbg), int(dbh), w, h, w // 2, h // 2, int(boot_to), 0)
# sbsim's struct blkman at 0x200: the block devices' host files (sbshm.h: 0 base, 1 overlay, 2 repo)
PRESENT, RO = 1, 2
bm = struct.pack('<Q', 0x5645444b4c424253)
for name, path, fl in (('base', base, PRESENT | RO), ('overlay', overlay, PRESENT), ('repo', repo, PRESENT | RO),
                       ('', '', 0)):
    p = os.path.abspath(path).encode() if path else b''
    assert len(p) < 228, 'path too long for the manifest: ' + path
    bm += struct.pack('<II20s228s', fl if path else 0, 0, name.encode(), p)
data = bytearray(off)
data[:len(m)] = m
data[0x200:0x200 + len(bm)] = bm
for o, b in zip(offs, blobs):
    data[o:o + len(b)] = b
open(out, 'wb').write(data)
print(f'run-desktop: store {len(data)} bytes, Image {len(blobs[0])}, DTB {len(blobs[1])}, initrd {len(blobs[2])}; '
      f'base {base}, overlay {overlay or "(absent)"}, repo {repo or "(absent)"}')
EOF
rm -f sbsim-*-run*.ppm
set +e
timeout "$TIMEOUT" qemu-system-aarch64 -M virt,gic-version=2,virtualization=off,secure=off \
    -cpu cortex-a53 -smp 8 -m 2048 -nographic -no-reboot \
    -semihosting-config enable=on,target=native \
    -kernel sbsim/sbsim.elf \
    -device loader,file=out/store-desktop.bin,addr=0x48000000,force-raw=on > "$LOG" 2>&1
set -e
for p in sbsim-*-run*.ppm; do [ -f "$p" ] && mv "$p" "out/$p"; done
{ grep -a 'sbsim: \|guest-init\|sbblk\|sbinit\|EXT4-fs\|erofs\|overlayfs' "$LOG" | cut -c1-220 | tail -n "${TAIL:-60}"; } || true
echo "== $(grep -a 'sbsim: RESULT' "$LOG" || echo 'sbsim: no RESULT line')"
grep -aq 'sbsim: RESULT PASS' "$LOG"
