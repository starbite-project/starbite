#!/bin/sh
# Starbite OS 1.2-alpha3: the terminal's three store entries for the phone (guest.c GUEST_TERM):
#   guest-term.img      the guest kernel of its own (linux/build-patched.sh GUEST=1: Image-guest, 7.1.0-sbguest)
#   guest-term.dtb      the S9 profile with the simple-framebuffer on a 720 x 1060 surface (2x above Starbite's
#                       keyboard: 1440 x 2120), fbcon in Terminus 16 x 32 (45 x 33 characters; guest.config a866f15),
#                       sbguest.mode=term (a shell on tty1), doorbell 511
#   guest-term-rd.img   busybox, guestd, sbguest.ko built against that kernel (run-qemu.sh BUILD_ONLY=1 recipe)
# The DTB carries this initrd's size: ship the three together. As probe/build.sh p4, with the surface and the font.
#   limactl shell starbite -- /Users/dev/project-starbite/starbite/guest/sbos/term-bundle.sh
# Environment: IMAGE (/mnt/build/sbguest/Image-guest), KOUT (/mnt/build/sbguest/gout), WORK (/mnt/build/sbguest-sbos/term),
# OUT ($WORK/bundle).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
IMAGE=${IMAGE:-/mnt/build/sbguest/Image-guest}
KOUT=${KOUT:-/mnt/build/sbguest/gout}
WORK=${WORK:-/mnt/build/sbguest-sbos/term}
OUT=${OUT:-$WORK/bundle}
[ -f "$IMAGE" ] || { echo "term-bundle: no $IMAGE (starbite/guest/linux/build-patched.sh GUEST=1)" >&2; exit 1; }
mkdir -p "$OUT"
BUILD_ONLY=1 WORK="$WORK" KOUT="$KOUT" "$here/../run-qemu.sh" mod > "$OUT/build.log" 2>&1 || { tail -20 "$OUT/build.log"; exit 1; }
rd="$WORK/out/initrd.cpio.gz"
python3 "$here/../mk-guest-dts.py" --profile s9 --rd-size "$(stat -c %s "$rd")" --db-guest 511 --fb 720x1060 --term \
    --bootargs-extra "fbcon=font:TER16x32" \
    "$OUT/guest-term.dts" >/dev/null
dtc -q -I dts -O dtb -o "$OUT/guest-term.dtb" "$OUT/guest-term.dts"
cp "$IMAGE" "$OUT/guest-term.img"
cp "$rd" "$OUT/guest-term-rd.img"
(cd "$OUT" && sha256sum guest-term.img guest-term.dtb guest-term-rd.img > SHA256SUMS && cat SHA256SUMS)
