#!/bin/sh
# Starbite OS Next, L3-L6 (docs/STARBITE-LINUX-APPS.md): the guest's three store files for the Linux apps build
# (kern/guest.c GUEST_WL with next-linux.patch + blk/guest-card.patch):
#   guest-wl.img      the desktop guest kernel (linux/build-patched.sh GUEST=1 GCONF=guest-desktop.config:
#                     EROFS, overlay, ext4, seccomp, namespaces, zram)
#   guest-wl.dtb      the S9 profile, sbguest.mode=desktop, doorbell 511
#   guest-wl-rd.img   busybox, sbguest.ko with sbblk (built against that kernel), init (the desktop root switch:
#                     /dev/sbblk0 base + sbblk1 overlay or tmpfs + sbblk2 repo -> /sbin/sbinit; with no base image the
#                     built-in terminal: sbwl + foot from this initramfs, as L2)
# The DTB carries this initrd's size: ship the three together.
#   limactl shell starbite -- /Users/dev/project-starbite/starbite/guest/next/desktop-bundle.sh
# Environment: WORK (/mnt/build/sbguest2/dk), KOUT (/mnt/build/sbguest2/gout), IMAGE (/mnt/build/sbguest2/Image-guest),
# OUT ($WORK/bundle).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-/mnt/build/sbguest2/dk}
KOUT=${KOUT:-/mnt/build/sbguest2/gout}
IMAGE=${IMAGE:-/mnt/build/sbguest2/Image-guest}
OUT=${OUT:-$WORK/bundle}
[ -f "$IMAGE" ] || { echo "desktop-bundle: no $IMAGE" >&2; exit 1; }
mkdir -p "$OUT" "$WORK/out"
WORK="$WORK" KOUT="$KOUT" "$here/../sbwl/mk-initrd.sh" > "$OUT/build.log" 2>&1 || { tail -20 "$OUT/build.log"; exit 1; }
rd="$WORK/out/initrd-wl.cpio.gz"
python3 "$here/../mk-guest-dts.py" --profile s9 --rd-size "$(stat -c %s "$rd")" --db-guest 511 \
    --bootargs-extra "sbguest.mode=desktop snd_dummy.enable=0" "$OUT/guest-wl.dts" >/dev/null
dtc -q -I dts -O dtb -o "$OUT/guest-wl.dtb" "$OUT/guest-wl.dts"
cp "$IMAGE" "$OUT/guest-wl.img"
cp "$rd" "$OUT/guest-wl-rd.img"
(cd "$OUT" && sha256sum guest-wl.img guest-wl.dtb guest-wl-rd.img > SHA256SUMS && cat SHA256SUMS)
