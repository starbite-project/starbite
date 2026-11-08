#!/bin/bash
# The Starbite Linux 7.1 Image for the M3 guest (docs/STARBITE-OS-LINUX-M3.md 2, 4), built in the Lima VM
# incrementally in a COPY of the S9 build's out/ (the shared tree's own out/ and Image stay as they are):
#   0001 irq-gic.c   starbite,shared-distributor (leave Starbite OS's GIC distributor alone)
#   0002 psci.c      starbite,cpu-on-trampoline (CPU_ON through a slot below 4 GiB: the S9 drops higher entries)
# Both do nothing without their DT properties. The source gets the patches only for the build; each file is
# checked unchanged before and restored (and checked by sha256) on exit.
#   limactl shell starbite -- /Users/dev/project-starbite/starbite/guest/linux/build-patched.sh
# Writes $WORK/patch/*.patch (also copied next to this script) and $WORK/Image-patched.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
S=${KSRC:-/mnt/build/src/mainline}
WORK=${WORK:-/mnt/build/sbguest}
K=$WORK/kout
PD=$WORK/patch
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- LC_ALL=C
# the release of the tree being copied (7.1.0-starbite-<id>): keep it, so modules built against out/ still load
rel=$(cat "$S/out/include/config/kernel.release")
export LOCALVERSION=-${rel##*-}
cd "$S"
git diff --quiet HEAD -- drivers/irqchip/irq-gic.c || { echo "irq-gic.c is not pristine v7.1: stop"; exit 1; }
grep -q S9P-VENDOR-DEFAULTS arch/arm64/kernel/psci.c || { echo "psci.c is not the port's: stop"; exit 1; }
rm -rf "$PD" && mkdir -p "$PD"
mk() {  # mk N FILE SCRIPT NAME
    mkdir -p "$PD/$1/a/$(dirname "$2")" "$PD/$1/b/$(dirname "$2")"
    cp "$2" "$PD/$1/a/$2"
    cp "$2" "$PD/$1/b/$2"
    python3 "$here/$3" "$PD/$1/b/$2"
    (cd "$PD/$1" && diff -u "a/$2" "b/$2" > "$PD/$4" || true)
    cp "$PD/$4" "$here/$4"
}
mk 1 drivers/irqchip/irq-gic.c mk-gic-patch.py 0001-irqchip-gic-starbite-shared-distributor.patch
mk 2 arch/arm64/kernel/psci.c mk-psci-patch.py 0002-arm64-psci-starbite-cpu-on-trampoline.patch
sums=$(sha256sum drivers/irqchip/irq-gic.c arch/arm64/kernel/psci.c)
[ -d "$K" ] || cp -a "$S/out" "$K"
restore() {
    cd "$S"
    cp "$PD/1/a/drivers/irqchip/irq-gic.c" drivers/irqchip/irq-gic.c
    cp "$PD/2/a/arch/arm64/kernel/psci.c" arch/arm64/kernel/psci.c
    if [ "$(sha256sum drivers/irqchip/irq-gic.c arch/arm64/kernel/psci.c)" = "$sums" ]; then
        echo "build-patched: irq-gic.c and psci.c restored"
    else
        echo "build-patched: RESTORE MISMATCH, check irq-gic.c / psci.c" >&2
    fi
}
trap restore EXIT
cp "$PD/1/b/drivers/irqchip/irq-gic.c" drivers/irqchip/irq-gic.c
cp "$PD/2/b/arch/arm64/kernel/psci.c" arch/arm64/kernel/psci.c
echo "build-patched: $(make -s O="$K" kernelrelease)"
time make -s -j"$(nproc)" O="$K" Image 2>&1 | grep -v 'warning: no previous prototype\|^ *[0-9]* | \|^ *| *\^' || true
cp "$K/arch/arm64/boot/Image" "$WORK/Image-patched"
for prop in starbite,shared-distributor starbite,cpu-on-trampoline; do
    grep -q "$prop" "$WORK/Image-patched" || { echo "build-patched: the Image lacks $prop"; exit 1; }
done
sha256sum "$WORK/Image-patched"
