#!/bin/bash
# Build the Starbite Linux 7.1 kernel for the Galaxy S9 (starlte) in the Lima VM.
#
#   limactl shell starbite -- bash /Users/dev/project-starbite/tools/linux/build-kernel.sh
#
# Plan: docs/LINUX-REBUILD-PLAN.md 2.2-2.3. Every run starts from a clean tree:
#   1. space check: VM disk and the Mac volume (/Users, shared into the VM)
#   2. mainline tree reset to v7.1 (8cd9520d), untracked files removed (out/ kept,
#      but FRESH_CONFIG=1 regenerates out/.config from defconfig)
#   3. tools/linux/port (vendored S9+ port) rsynced to the work copy ~/linux/work,
#      plus the phone's Broadcom firmware from build/kiosk (sha256-checked)
#   4. mk-starlte-dts.py (S9 DTS), patch-mainline.py --port (work copy) and
#      --tree (mainline), then the port's build71.sh, which merges
#      tools/linux/config/starbite.config and runs check-config.py
#   5. modules -> an ext4 image named after `make kernelrelease`
#   6. the rollback DTB (linux-old.dtb: --legacy-cma), check-dtb.py on both
#   7. outputs to ~/linux/out/starbite (VM), the modules image also to
#      ~/distros/_kernel, then a copy to the Mac's build/linux/kernel-<release>
#
# Everything big stays on the VM's own disk (WORK, DIST, BUILD). Older
# m71-* dist folders this script made are deleted after the copy.
set -euo pipefail
PROJ=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
L=$PROJ/tools/linux
MAIN=${STARBITE_MAIN:-/Users/dev/project-starbite}	# main checkout (git-ignored build/)
FW_SRC=${FW_SRC:-$MAIN/build/linux-bundle/firmware}
export BUILD=${BUILD:-/mnt/build}
SRC=$BUILD/src/mainline
BASE=8cd9520d35a6
WORK=${WORK:-$HOME/linux/work}
export DIST=${DIST:-$WORK/dist}
OUT=${OUT:-$HOME/linux/out/starbite}
KDIR=${KDIR:-$HOME/distros/_kernel}
MAC_OUT=${MAC_OUT:-$MAIN/build/linux}
MIN_VM_GB=${MIN_VM_GB:-8}
MIN_MAC_GB=${MIN_MAC_GB:-3}
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
die() { echo "build-kernel: $*" >&2; exit 1; }
free_gb() { df -Pk "$1" | awk 'NR == 2 { print int($4 / 1048576) }'; }

# ---- 1. where things go, and is there room ----------------------------------
for d in "$WORK" "$DIST" "$OUT" "$KDIR" "$BUILD"; do
	case "$d" in /Users/*) die "$d is on the Mac volume: keep WORK, DIST, OUT, KDIR and BUILD on the VM disk" ;; esac
done
[ "$(free_gb "$HOME")" -ge "$MIN_VM_GB" ] || die "VM disk: $(free_gb "$HOME") GB free, need $MIN_VM_GB"
if [ -z "${NO_MAC_COPY:-}" ]; then
	[ -d "$MAIN" ] || die "no $MAIN (set STARBITE_MAIN, or NO_MAC_COPY=1)"
	[ "$(free_gb "$MAIN")" -ge "$MIN_MAC_GB" ] ||
		die "Mac volume: $(free_gb "$MAIN") GB free, need $MIN_MAC_GB (free space, or NO_MAC_COPY=1)"
fi
if [ -n "${NO_MAC_COPY:-}" ]; then echo "space: VM $(free_gb "$HOME") GB free, no Mac copy"
else echo "space: VM $(free_gb "$HOME") GB free, Mac $(free_gb "$MAIN") GB free"; fi

# ---- 2. clean mainline tree --------------------------------------------------
[ -d "$SRC/.git" ] || die "no mainline tree at $SRC (plan 2.2: port/scripts/build/fetch_mainline.sh)"
mkdir -p "$DIST"
if [ -n "$(git -C "$SRC" status --porcelain --untracked-files=no)" ]; then
	git -C "$SRC" diff --binary > "$DIST/pre-reset-$STAMP.patch"
	echo "tree was dirty: its diff is kept in $DIST/pre-reset-$STAMP.patch"
fi
git -C "$SRC" reset -q --hard "$BASE"
git -C "$SRC" clean -q -fdx -e /out
[ "$(git -C "$SRC" rev-parse HEAD)" = "$(git -C "$SRC" rev-parse "$BASE^{commit}")" ] || die "tree is not at $BASE"

# ---- 3. work copy of the port, firmware ----------------------------------------
mkdir -p "$WORK"
rsync -a --delete --exclude /dist --exclude /build-logs "$L/port/" "$WORK/"
for f in $(awk '{ print $2 }' "$WORK/firmware/broadcom.sha256"); do
	[ -f "$FW_SRC/$f" ] || die "missing $FW_SRC/$f (the phone's vendor firmware, see tools/linux/port/STARBITE.md)"
	install -m644 "$FW_SRC/$f" "$WORK/firmware/$f"
done
(cd "$WORK/firmware" && sha256sum --quiet -c broadcom.sha256) || die "Broadcom firmware does not match broadcom.sha256"

# ---- 4. DTS, Starbite edits, the port's build ----------------------------------
BOOTARGS=$(python3 "$L/mk-starlte-dts.py" --print-bootargs)
python3 "$L/mk-starlte-dts.py" ${DTS_ARGS:-} "$WORK/src/mainline-dts/exynos9810-star2lte.dts" \
	"$WORK/src/mainline-dts/exynos9810-starlte.dts"
python3 "$L/patch-mainline.py" --port "$WORK" --tree "$SRC"

# GPU = Mali-G72 (Panfrost), DECON = the port's KMS scanout driver, WLAN =
# BCM4361 over PCIe with the phone's own firmware built in.
export UFS=1 GADGET=configfs GPU=1 DECON=1 WLAN=1 FRESH_CONFIG=1
export LOCALVERSION=	# set but empty: no "+" after the release (CONFIG_LOCALVERSION="-starbite")
export BOOT_CMDLINE="$BOOTARGS"	# CONFIG_CMDLINE, only the fallback with CMDLINE_FROM_BOOTLOADER
export STARBITE_CONFIG="$L/config/starbite.config" STARBITE_CHECK_CONFIG="$L/check-config.py"
LOG71=$DIST/build-kernel-$STAMP.log
bash "$WORK/scripts/build/build71.sh" "$@" 2>&1 | tee "$LOG71"
KDIST=$(sed -n 's/^BUILD71_OK //p' "$LOG71" | tail -1)
[ -n "$KDIST" ] && [ -f "$KDIST/.starbite-build" ] || die "build71.sh did not finish (log: $LOG71)"

export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- LC_ALL=C
K() { make -C "$SRC" O=out "$@"; }
python3 "$L/check-config.py" --bootargs "$BOOTARGS" "$SRC/out/.config" "$STARBITE_CONFIG"
KREL=$(K -s kernelrelease)
case "$KREL" in *-starbite) ;; *) die "kernelrelease is $KREL, want *-starbite" ;; esac
echo "kernelrelease: $KREL"

# the 28 mainline files the port changes (tools/linux/patches/): none may go missing
want=$(grep '^diff --git' "$L/patches/0000-reference-kernel-source.patch" | awk '{ print $3 }' | sort)
have=$(grep '^diff --git' "$KDIST/kernel-source.patch" | awk '{ print $3 }' | sort)
missing=$(comm -23 <(echo "$want") <(echo "$have"))
[ -z "$missing" ] || die "files the reference patch changes but this build does not: $missing"
echo "kernel-source.patch: $(echo "$have" | wc -l) files (reference: $(echo "$want" | wc -l))"

# ---- 5. modules image ------------------------------------------------------------
K -s -j"$(nproc)" modules >>"$LOG71" 2>&1 || die "modules failed (log: $LOG71)"
MODS=$BUILD/out/mods
sudo rm -rf "$MODS"
K -s modules_install INSTALL_MOD_PATH="$MODS" INSTALL_MOD_STRIP=1 >>"$LOG71" 2>&1 || die "modules_install failed"
MD=$MODS/lib/modules/$KREL
[ -f "$MD/modules.dep" ] || die "no $MD/modules.dep (is kmod/depmod installed?)"
rm -f "$MD/build" "$MD/source"
NMOD=$(find "$MD" -name '*.ko*' | wc -l)
SIZE_MB=$(( $(du -sm "$MD" | cut -f1) * 5 / 4 + 16 ))
MODIMG=$BUILD/out/modules-$KREL.img
rm -f "$MODIMG"
sudo chown -R 0:0 "$MD"
sudo mke2fs -q -t ext4 -O ^has_journal -m 0 -L sb-modules -d "$MD" "$MODIMG" "${SIZE_MB}M"
sudo chown "$(id -u):$(id -g)" "$MODIMG"
sudo rm -rf "$MODS"
echo "modules: $NMOD in $MODIMG (${SIZE_MB} MiB)"

# ---- 6. rollback DTB and the checks ------------------------------------------------
# linux-old.dtb = the final in-tree DTS (after build71.sh's UFS/GPU/DECON/WLAN
# edits) with only the CMA moved back to the port's proven base; the difference
# is taken from mk-starlte-dts.py itself and applied with patch(1), so it fails
# loudly if anything else differs.
DTSD=$SRC/arch/arm64/boot/dts/exynos
DTBD=$SRC/out/arch/arm64/boot/dts/exynos
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
python3 "$L/mk-starlte-dts.py" ${DTS_ARGS:-} "$L/port/src/mainline-dts/exynos9810-star2lte.dts" "$TMP/new.dts" >/dev/null
python3 "$L/mk-starlte-dts.py" ${DTS_ARGS:-} --legacy-cma "$L/port/src/mainline-dts/exynos9810-star2lte.dts" "$TMP/old.dts" >/dev/null
diff -u "$TMP/new.dts" "$TMP/old.dts" > "$TMP/cma.patch" && die "--legacy-cma changed nothing"
cp "$DTBD/exynos9810-starlte.dtb" "$TMP/linux.dtb"
cp "$DTSD/exynos9810-starlte.dts" "$TMP/final.dts"
patch -s "$DTSD/exynos9810-starlte.dts" < "$TMP/cma.patch"
K -s dtbs >>"$LOG71" 2>&1 || { cp "$TMP/final.dts" "$DTSD/exynos9810-starlte.dts"; die "rollback DTB failed"; }
cp "$DTBD/exynos9810-starlte.dtb" "$TMP/linux-old.dtb"
cp "$TMP/final.dts" "$DTSD/exynos9810-starlte.dts"
K -s dtbs >>"$LOG71" 2>&1
cmp -s "$DTBD/exynos9810-starlte.dtb" "$TMP/linux.dtb" || die "DTB differs after restoring the DTS"

IMAGE=$SRC/out/arch/arm64/boot/Image
python3 "$L/check-dtb.py" --bootargs "$BOOTARGS" --image "$IMAGE" "$TMP/linux.dtb"
python3 "$L/check-dtb.py" --bootargs "$BOOTARGS" --image "$IMAGE" --legacy-cma "$TMP/linux-old.dtb"

# ---- 7. outputs ----------------------------------------------------------------------
rm -rf "$OUT"
mkdir -p "$OUT" "$KDIR"
cp "$IMAGE" "$OUT/Image"
cp "$TMP/linux.dtb" "$OUT/exynos9810-starlte.dtb"
cp "$TMP/linux-old.dtb" "$OUT/linux-old.dtb"
cp "$SRC/out/.config" "$OUT/kernel.config"
cp "$SRC/out/System.map" "$OUT/System.map"
mv "$MODIMG" "$OUT/modules-$KREL.img"
echo "$KREL" > "$OUT/kernelrelease"
cp "$KDIST/kernel-source.patch" "$OUT/"
(cd "$OUT" && sha256sum Image exynos9810-starlte.dtb linux-old.dtb kernel.config "modules-$KREL.img" > SHA256SUMS)
cp "$OUT/modules-$KREL.img" "$KDIR/modules-$KREL.img"

# older dist folders made by this script (the .starbite-build marker); the
# port's own m71-* folders from before are left alone
for d in "$DIST"/m71-*/; do
	d=${d%/}
	if [ -f "$d/.starbite-build" ] && [ "$d" != "$KDIST" ]; then rm -rf "$d"; echo "pruned $d"; fi
done
{ ls -1t "$DIST"/build-kernel-*.log | tail -n +4 | xargs -r rm -f; } || true

if [ -z "${NO_MAC_COPY:-}" ]; then
	need=$(( $(du -sm "$OUT" | cut -f1) / 1024 + MIN_MAC_GB ))
	if [ "$(free_gb "$MAIN")" -ge "$need" ]; then
		M=$MAC_OUT/kernel-$KREL
		rm -rf "$M"
		mkdir -p "$M"
		cp "$OUT"/Image "$OUT"/exynos9810-starlte.dtb "$OUT"/linux-old.dtb "$OUT"/kernel.config \
			"$OUT/modules-$KREL.img" "$OUT"/kernelrelease "$OUT"/SHA256SUMS "$M/"
		(cd "$M" && sha256sum --quiet -c SHA256SUMS) || die "Mac copy does not match SHA256SUMS"
		echo "Mac copy: $M"
	else
		echo "Mac copy skipped: $(free_gb "$MAIN") GB free on the Mac, need $need"
	fi
fi
ls -l "$OUT"
echo "STARBITE_KERNEL_OK $KREL $OUT"
