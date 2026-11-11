#!/bin/sh
# Starbite OS Next, L2 (docs/STARBITE-LINUX-APPS.md): the Linux apps' three store entries for the phone
# (kern/guest.c GUEST_WL, the Terminal app's window in Next through starbite/guest/next/gwin.c):
#   guest-wl.img        the guest kernel of its own (linux/build-patched.sh GUEST=1: Image-guest, 7.1.0-sbguest)
#   guest-wl.dtb        the S9 profile, sbguest.mode=wayland (sbwl + foot; no framebuffer node: sbwl reads the
#                       surface size, 720 x 916, from the shared header), doorbell 511
#   guest-wl-rd.img     busybox, sbguest.ko, sbwl, foot and their libraries, fonts, xkb (sbwl/mk-initrd.sh)
# The DTB carries this initrd's size: ship the three together.
#   limactl shell starbite -- /Users/dev/project-starbite/starbite/guest/next/wl-bundle.sh
# Environment: IMAGE (/mnt/build/sbguest/Image-guest), WORK (/mnt/build/sbguest), OUT (/mnt/build/sbguest-next/bundle).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-/mnt/build/sbguest}
IMAGE=${IMAGE:-$WORK/Image-guest}
OUT=${OUT:-/mnt/build/sbguest-next/bundle}
[ -f "$IMAGE" ] || { echo "wl-bundle: no $IMAGE (starbite/guest/linux/build-patched.sh GUEST=1)" >&2; exit 1; }
mkdir -p "$OUT"
WORK="$WORK" "$here/../sbwl/mk-initrd.sh" > "$OUT/build.log" 2>&1 || { tail -20 "$OUT/build.log"; exit 1; }
rd="$WORK/out/initrd-wl.cpio.gz"
python3 "$here/../mk-guest-dts.py" --profile s9 --rd-size "$(stat -c %s "$rd")" --db-guest 511 \
    --bootargs-extra "sbguest.mode=wayland" "$OUT/guest-wl.dts" >/dev/null
dtc -q -I dts -O dtb -o "$OUT/guest-wl.dtb" "$OUT/guest-wl.dts"
cp "$IMAGE" "$OUT/guest-wl.img"
cp "$rd" "$OUT/guest-wl-rd.img"
(cd "$OUT" && sha256sum guest-wl.img guest-wl.dtb guest-wl-rd.img > SHA256SUMS && cat SHA256SUMS)
