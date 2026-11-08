#!/bin/bash
# The Starbite Linux 7.1 Image with the shared-distributor GIC patch (docs/STARBITE-OS-LINUX-M3.md 4), built in
# the Lima VM incrementally in a COPY of the S9 build's out/ (the shared tree's own out/ and Image stay as they
# are). The source gets the patch only for the build and is reverted on exit (checked pristine before).
#   limactl shell starbite -- /Users/dev/project-starbite/starbite/guest/linux/build-patched.sh
# Writes $WORK/patch/0001-...patch (also copied next to this script) and $WORK/Image-patched.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
S=${KSRC:-/mnt/build/src/mainline}
WORK=${WORK:-/mnt/build/sbguest}
K=$WORK/kout
PD=$WORK/patch
PATCH=0001-irqchip-gic-starbite-shared-distributor.patch
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- LC_ALL=C
# the release of the tree being copied (7.1.0-starbite-<id>): keep it, so modules built against out/ still load
rel=$(cat "$S/out/include/config/kernel.release")
export LOCALVERSION=-${rel##*-}
cd "$S"
git diff --quiet HEAD -- drivers/irqchip/irq-gic.c || { echo "irq-gic.c is not pristine v7.1: stop"; exit 1; }
rm -rf "$PD" && mkdir -p "$PD/a/drivers/irqchip" "$PD/b/drivers/irqchip"
cp drivers/irqchip/irq-gic.c "$PD/a/drivers/irqchip/"
cp drivers/irqchip/irq-gic.c "$PD/b/drivers/irqchip/"
python3 "$here/mk-gic-patch.py" "$PD/b/drivers/irqchip/irq-gic.c"
(cd "$PD" && diff -u a/drivers/irqchip/irq-gic.c b/drivers/irqchip/irq-gic.c > "$PATCH" || true)
cp "$PD/$PATCH" "$here/$PATCH"
[ -d "$K" ] || cp -a "$S/out" "$K"
git apply "$PD/$PATCH"
trap 'cd "$S" && git apply -R "$PD/$PATCH" && git diff --quiet HEAD -- drivers/irqchip/irq-gic.c && echo "build-patched: irq-gic.c reverted, pristine"' EXIT
echo "build-patched: $(make -s O="$K" kernelrelease)"
time make -s -j"$(nproc)" O="$K" Image
cp "$K/arch/arm64/boot/Image" "$WORK/Image-patched"
n=$(grep -c "starbite,shared-distributor" "$WORK/Image-patched" || true)
[ "$n" -ge 1 ] || { echo "build-patched: the Image lacks the patch"; exit 1; }
sha256sum "$WORK/Image-patched"
