#!/bin/bash
# Build the Starbite Linux 7.1 kernel for the Galaxy S9 (starlte) in the Lima VM,
# from a clean mainline tree and the vendored port (docs/LINUX-REBUILD-PLAN.md 2).
#
#   limactl shell starbite -- /Users/dev/project-starbite/tools/linux/build-kernel.sh
#
# Inputs, all in this repo:
#   tools/linux/port/            SheroAbi/galaxy-s9plus-mainline-linux @ bcee6014fc2e
#                                (drivers, build scripts, star2lte DTS, firmware)
#   tools/linux/patch-mainline.py  Starbite's edits: --port on the work copy
#                                (DECON early_event, G3D rail check, cpufreq 5 ms,
#                                build71.sh hooks), --tree on mainline (dw_mmc,
#                                no S9P_GEM_DMA32, Panfrost autosuspend 1 s)
#   tools/linux/mk-starlte-dts.py  S9 DTS: 4 GB, 3000 mAh, /chosen bootargs and
#                                initrd placeholders, CMA out of the loader window, microSD
#   tools/linux/config/starbite.config  merged after the port's config, then checked
# Order: reset mainline to v7.1 (git reset --hard + clean; out/ is kept so the
# build is incremental, CLEAN=1 drops it too), work copy of the port, patches,
# DTS, the port's build71.sh, modules, the modules image, checks.
#
# Outputs:
#   $OUT ($HOME/distros/_kernel)   Image, exynos9810-starlte.dtb, linux-old.dtb (rollback,
#                                  mk-starlte-dts.py --legacy-cma), kernel.config, release,
#                                  modules-<release>.img (ext4, label sb-modules), SHA256SUMS
#   $REPO_OUT ($PROJ/build/linux)  the same without the modules image, plus
#                                  kernel-source.patch (small files only)
# Release: <version>-starbite-<build id>, the id a hash of every input that
# shapes the kernel (port, patch-mainline.py, starbite.config, this script, its
# arguments). The init mounts /distros/_kernel/modules-$(uname -r).img, so a
# modules image only ever meets the kernel it was built with; the image also
# holds /starbite-kernel ("STARBITE-KERNEL <release> <Image SHA256>", checked
# by check-kernel.py pair Image modules-<release>.img before it is copied).
# Environment: CLEAN=1, GPU_FLOOR_455=1 (no 260 MHz OPP), NO_MODULES=1,
# MIN_FREE_GB (VM, default 3), MIN_HOST_FREE_GB (Mac, default 5),
# BUILD (default /mnt/build: src/mainline = the v7.1 clone, out/busybox-arm64).
# A second BUILD can share the clone's objects:
#   git -C /mnt/build/src/mainline worktree add --detach /mnt/build/starbite/src/mainline 8cd9520d35a6
#   mkdir -p /mnt/build/starbite/out && cp /mnt/build/out/busybox-arm64 /mnt/build/starbite/out/
set -euo pipefail
PROJ=${PROJ:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}
LINUX=$PROJ/tools/linux
export BUILD=${BUILD:-/mnt/build}
SRC=$BUILD/src/mainline
BASE=8cd9520d35a6                       # v7.1, what the port was tested on
WORK=${WORK:-$HOME/linux/starbite-work}
OUT=${OUT:-$HOME/distros/_kernel}
REPO_OUT=${REPO_OUT:-$PROJ/build/linux}
MIN_FREE_GB=${MIN_FREE_GB:-3}
MIN_HOST_FREE_GB=${MIN_HOST_FREE_GB:-5}
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
LOG=$WORK-logs/build-$STAMP.log
mkdir -p "$WORK-logs"
# One build per tree: a second one resets and patches the sources under the first.
exec 9>"$BUILD/.starbite-build.lock"
flock -n 9 || { echo "another build-kernel.sh is running on $BUILD"; exit 1; }
# And one build at a time across trees: WORK, its logs, OUT and REPO_OUT are
# the same for every BUILD (rsync --delete on WORK, outputs overwritten).
mkdir -p "$HOME/linux"
exec 8>"$HOME/linux/.starbite-build.lock"
flock -n 8 || { echo "another build-kernel.sh is running (shared $WORK, $OUT, $REPO_OUT)"; exit 1; }

# The VM disk is a sparse file on the Mac: every byte written here costs Mac
# space and is never given back. Stop before either side runs short.
space() {
	local vm host
	vm=$(df -Pk "$BUILD" | awk 'NR==2 {print int($4 / 1048576)}')
	host=$(df -Pk "$PROJ" | awk 'NR==2 {print int($4 / 1048576)}')
	echo "--- free: VM ${vm} GiB, host ${host} GiB ($1)"
	[ "$vm" -ge "$MIN_FREE_GB" ] || { echo "VM disk below ${MIN_FREE_GB} GiB, stopping"; exit 1; }
	[ "$host" -ge "$MIN_HOST_FREE_GB" ] || { echo "host disk below ${MIN_HOST_FREE_GB} GiB, stopping"; exit 1; }
}

for t in git rsync python3 dtc fdtput mke2fs e2fsck aarch64-linux-gnu-gcc qemu-aarch64-static depmod; do
	command -v "$t" >/dev/null || { echo "missing $t (plan 2.2 VM setup)"; exit 1; }
done
[ -x "$BUILD/out/busybox-arm64" ] || [ -f "$BUILD/out/busybox-arm64" ] ||
	{ echo "missing $BUILD/out/busybox-arm64: python3 $LINUX/port/scripts/build/fetch_busybox.py $BUILD/out/busybox-arm64"; exit 1; }
space start

echo "=== mainline v7.1, clean ==="
git -C "$SRC" rev-parse --verify -q "$BASE^{commit}" >/dev/null || { echo "$SRC lacks $BASE: run port/scripts/build/fetch_mainline.sh"; exit 1; }
git -C "$SRC" reset -q --hard "$BASE"
if [ -n "${CLEAN:-}" ]; then
	git -C "$SRC" clean -fdxq
else
	git -C "$SRC" clean -fdxq -e /out
fi
[ -z "$(git -C "$SRC" status --porcelain --ignored -- . ':!out')" ] || { echo "tree not clean after reset"; exit 1; }

echo "=== work copy of tools/linux/port ==="
rsync -a --delete "$LINUX/port/" "$WORK/"
# build71.sh INIT=boot packs this into an initramfs; starbite.config sets
# INITRAMFS_SOURCE="" so the kernel does not carry it (the initrd is the
# store file linux.initrd), but the port's script still wants the file.
install -D -m755 "$LINUX/init" "$WORK/device/mainline-init-boot"
python3 "$LINUX/patch-mainline.py" --port "$WORK"
DTS_FLAGS=()
[ -z "${GPU_FLOOR_455:-}" ] || DTS_FLAGS+=(--gpu-floor-455)
python3 "$LINUX/mk-starlte-dts.py" "${DTS_FLAGS[@]}" "$WORK/src/mainline-dts/exynos9810-star2lte.dts" \
	"$WORK/src/mainline-dts/exynos9810-starlte.dts"
python3 "$LINUX/patch-mainline.py" --tree "$SRC"

echo "=== port build (build71.sh) ==="
# GPU = Mali-G72 (Panfrost), DECON = the KMS scanout driver, WLAN = BCM4361
# over PCIe with the phone's own firmware built in (port/firmware).
export INIT=boot UFS=1 GADGET=configfs GPU=1 DECON=1 WLAN=1 FRESH_CONFIG=1
export DIST=$BUILD/out/dist-starbite
# CONFIG_CMDLINE = the DTB's bootargs, used only if a loader passes none
BOOT_CMDLINE=$(python3 "$LINUX/mk-starlte-dts.py" --print-bootargs)
export BOOT_CMDLINE
export STARBITE_CONFIG=$LINUX/config/starbite.config STARBITE_CHECK=$LINUX/check-kernel.py
rm -rf "$DIST"
# microSD: DW MSHC with the Exynos glue, fixed rails, FAT/exFAT
STARBITE_CFG=("--enable MMC" "--enable MMC_BLOCK" "--enable MMC_DW" "--enable MMC_DW_PLTFM"
	"--enable MMC_DW_EXYNOS" "--enable REGULATOR" "--enable REGULATOR_FIXED_VOLTAGE"
	"--enable VFAT_FS" "--enable EXFAT_FS" "--enable NLS_CODEPAGE_437" "--enable NLS_ISO8859_1")
# Build id (see Release above). LOCALVERSION also keeps setlocalversion's "+"
# off: the release is exactly <version>-starbite-<id>.
BUILD_ID=$( {
	echo "$BASE"
	printf '%s\n' "$BOOT_CMDLINE" "${STARBITE_CFG[@]}" "$@"
	cd "$LINUX" && find port config/starbite.config patch-mainline.py build-kernel.sh -type f -print0 |
		LC_ALL=C sort -z | xargs -0 sha256sum
} | sha256sum | cut -c1-8)
export LOCALVERSION=-$BUILD_ID
echo "build id $BUILD_ID"
bash "$WORK/scripts/build/build71.sh" "${STARBITE_CFG[@]}" "$@"
space "after Image"

cd "$SRC"
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- LC_ALL=C
KREL=$(make -s O=out kernelrelease)
echo "release $KREL"
case "$KREL" in *-starbite-"$BUILD_ID") ;; *) echo "unexpected release $KREL (build id $BUILD_ID)"; exit 1 ;; esac
KID=$(python3 "$LINUX/check-kernel.py" id out/arch/arm64/boot/Image)
echo "$KID"

mkdir -p "$OUT"
# modules images of other releases (other builds) must not sit next to this Image
find "$OUT" -maxdepth 1 -name 'modules-*.img' ! -name "modules-$KREL.img" -print -delete
if [ -z "${NO_MODULES:-}" ]; then
	echo "=== modules ==="
	if ! make -j"$(nproc)" O=out modules >"$LOG" 2>&1; then
		grep -nE 'error:|Error [0-9]' "$LOG" | head -30
		exit 1
	fi
	space "after modules"
	STAGE=$BUILD/out/mods-starbite
	rm -rf "$STAGE"
	make -s O=out modules_install INSTALL_MOD_PATH="$STAGE" INSTALL_MOD_STRIP=1 >>"$LOG" 2>&1
	MODDIR=$STAGE/lib/modules/$KREL
	rm -f "$MODDIR/build" "$MODDIR/source"
	echo "$KID" > "$MODDIR/starbite-kernel"     # check-kernel.py pair
	NMOD=$(find "$MODDIR" -name '*.ko*' | wc -l)
	USED=$(du -sm "$MODDIR" | cut -f1)
	SIZE=$(( USED * 13 / 10 + 32 ))
	[ "$SIZE" -ge 256 ] || SIZE=256
	IMG=$OUT/modules-$KREL.img
	rm -f "$IMG"
	# root-owned files in the image (fakeroot when present; read-only either way)
	if command -v fakeroot >/dev/null; then
		fakeroot sh -c "chown -R 0:0 '$MODDIR' && mke2fs -q -t ext4 -L sb-modules -m 0 -d '$MODDIR' '$IMG' ${SIZE}M"
	else
		mke2fs -q -t ext4 -L sb-modules -m 0 -E root_owner=0:0 -d "$MODDIR" "$IMG" "${SIZE}M"
	fi
	e2fsck -fn "$IMG" >/dev/null
	rm -rf "$STAGE"
	echo "modules: $NMOD in $IMG (${USED} MiB in a ${SIZE} MiB ext4)"
fi

echo "=== outputs ==="
cp out/arch/arm64/boot/Image "$OUT/Image"
cp out/arch/arm64/boot/dts/exynos/exynos9810-starlte.dtb "$OUT/"
# linux-old.dtb, the rollback DTB (hidden catalog entry, same Image and
# initrd): this DTB with only the CMA region at mk-starlte-dts.py --legacy-cma
# (0x98000000, 256 MiB, ends at OS_RD). Same size, so fdtput rewrites it in place.
read -r CMA_OLD_BASE CMA_OLD_SIZE < <(python3 "$LINUX/mk-starlte-dts.py" --legacy-cma --print-cma)
cp "$OUT/exynos9810-starlte.dtb" "$OUT/linux-old.dtb"
fdtput -t x "$OUT/linux-old.dtb" /reserved-memory/linux,cma reg 0 "${CMA_OLD_BASE#0x}" "${CMA_OLD_SIZE#0x}"
NDIFF=$(diff <(dtc -q -I dtb -O dts "$OUT/exynos9810-starlte.dtb") <(dtc -q -I dtb -O dts "$OUT/linux-old.dtb") | grep -c '^[<>]' || true)
[ "$NDIFF" = 2 ] || { echo "linux-old.dtb differs in $NDIFF lines, expected only the CMA reg"; exit 1; }
cp out/.config "$OUT/kernel.config"
echo "$KREL" > "$OUT/release"
mkdir -p "$REPO_OUT"
cp "$OUT/Image" "$OUT/exynos9810-starlte.dtb" "$OUT/linux-old.dtb" "$OUT/kernel.config" "$OUT/release" "$REPO_OUT/"
B71=$(ls -d "$DIST"/m71-* 2>/dev/null | tail -1)
[ -z "$B71" ] || cp "$B71/kernel-source.patch" "$REPO_OUT/kernel-source.patch"
rm -rf "$DIST"                  # vmlinux, boot.img, dt.img: not used by the Starbite loader
(cd "$OUT" && sha256sum Image exynos9810-starlte.dtb linux-old.dtb kernel.config release modules-*.img 2>/dev/null > SHA256SUMS)
(cd "$REPO_OUT" && sha256sum Image exynos9810-starlte.dtb linux-old.dtb kernel.config release > SHA256SUMS)

echo "=== checks ==="
python3 "$LINUX/check-kernel.py" image "$OUT/Image"
python3 "$LINUX/check-kernel.py" dtb "$OUT/exynos9810-starlte.dtb"
python3 "$LINUX/check-kernel.py" dtb --legacy-cma --image "$OUT/Image" "$OUT/linux-old.dtb"
python3 "$LINUX/check-kernel.py" config "$OUT/kernel.config" "$STARBITE_CONFIG"
if [ -f "$OUT/modules-$KREL.img" ]; then
	python3 "$LINUX/check-kernel.py" pair "$OUT/Image" "$OUT/modules-$KREL.img"
else
	echo "no modules-$KREL.img (NO_MODULES): build the modules for this Image before copying it to the card"
fi
dtc -q -I dtb -O dts "$OUT/exynos9810-starlte.dtb" | grep -E 'bootargs|linux,initrd|starbite,rtc-epoch'
ls -l "$OUT" "$REPO_OUT/Image" "$REPO_OUT/exynos9810-starlte.dtb" "$REPO_OUT/linux-old.dtb"
space end
echo STARBITE_KERNEL_OK "$KREL"
