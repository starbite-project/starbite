#!/bin/bash
# Build mainline Linux 7.1 for the Galaxy S9 (starlte) in the lima VM.
#
#   limactl shell starbite -- bash /Users/dev/project-starbite/tools/linux/build-kernel.sh
#
# Uses SheroAbi/galaxy-s9plus-mainline-linux (S9+, star2lte; cloned to
# ~/linux/s9p) for its Exynos 9810 drivers and build, with Starbite's changes in
# a work copy (~/linux/work):
#   - device tree: tools/linux/mk-starlte-dts.py (S9 memory, battery, model),
#     built as exynos9810-starlte.dtb in place of the minimal upstream one
#   - initramfs init: tools/linux/init (UFS read-only, USB shell), never the
#     port's own, which mounts sda25 = our USERDATA (iOS + Kiosk) read-write
#   - command line: kernel log on the panel for bring-up
# Output: ~/linux/out/{Image,exynos9810-starlte.dtb,kernel.config}; the host
# copies them out (tools/linux/sbstore.py packs the OS store).
set -euo pipefail
PROJ=/Users/dev/project-starbite
S9P=$HOME/linux/s9p
WORK=$HOME/linux/work
OUT=$HOME/linux/out
export BUILD=${BUILD:-/mnt/build}

rsync -a --delete --exclude .git "$S9P/" "$WORK/"
python3 "$PROJ/tools/linux/mk-starlte-dts.py" "$S9P/src/mainline-dts/exynos9810-star2lte.dts" \
	"$WORK/src/mainline-dts/exynos9810-starlte.dts"
install -m755 "$PROJ/tools/linux/init" "$WORK/device/mainline-init-boot"
# Every star2lte name in the build becomes starlte: the dtb then replaces the
# upstream exynos9810-starlte.dtb target, so the dts Makefile needs no edit.
sed -i 's/star2lte/starlte/g' "$WORK/scripts/build/build71.sh"
grep -q 'exynos9810-starlte.dts' "$WORK/scripts/build/build71.sh"

python3 "$PROJ/tools/linux/patch-mainline.py" "$BUILD/src/mainline"

export INIT=boot UFS=1 GADGET=configfs
export DIST="$WORK/dist"
export BOOT_CMDLINE="${BOOT_CMDLINE:-console=ramoops-1 console=tty0 loglevel=7 panic=10 earlycon=s9pram keep_bootcon maxcpus=8 s9p_late_ramlog s9p_vendor_m3_defaults}"
# microSD (Starbite): DW MSHC with the Exynos glue, fixed rails, FAT/exFAT
STARBITE_CFG=("--enable MMC" "--enable MMC_BLOCK" "--enable MMC_DW" "--enable MMC_DW_PLTFM"
	"--enable MMC_DW_EXYNOS" "--enable REGULATOR" "--enable REGULATOR_FIXED_VOLTAGE"
	"--enable VFAT_FS" "--enable EXFAT_FS" "--enable NLS_CODEPAGE_437" "--enable NLS_ISO8859_1")
bash "$WORK/scripts/build/build71.sh" "${STARBITE_CFG[@]}" "$@"

SRC=$BUILD/src/mainline
mkdir -p "$OUT"
cp "$SRC/out/arch/arm64/boot/Image" "$OUT/Image"
cp "$SRC/out/arch/arm64/boot/dts/exynos/exynos9810-starlte.dtb" "$OUT/"
cp "$SRC/out/.config" "$OUT/kernel.config"
ls -l "$OUT"
echo STARBITE_KERNEL_OK
