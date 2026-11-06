#!/usr/bin/env bash
# Builds the Starbite installer environment (docs/INSTALLER.md 13) in the arm64 Lima VM, like
# tools/linux/mk-initrd.sh and tools/rescue/mk-rescue-initrd.sh:
#
#   limactl shell starbite -- sudo bash /Users/dev/project-starbite/tools/installer/env/mk-env.sh [OUTDIR]
#
# OUTDIR (default ~/distros/out/installer-env in the VM) gets:
#   installer.initrd  Alpine minirootfs + e2fsprogs exfatprogs f2fs-tools sgdisk(gptfdisk) dropbear
#                     util-linux (blockdev, losetup) coreutils (dd conv=fsync, head -c, sha256sum),
#                     Ubuntu's static busybox (devmem udhcpd), and init / sb-byname / getprop / sb-reboot
#                     from this directory. newc cpio | zstd -19 (COMP=gzip for a kernel without RD_ZSTD).
#   installer.image   a copy of the daily Linux 7.1 Image (tools/linux/build-kernel.sh: UFS exynos9810-ufs,
#                     the microSD, DWC3 gadget + configfs ECM/ACM; must have CONFIG_USB_CONFIGFS_ECM=y,
#                     CONFIG_USB_CONFIGFS_ACM=y, CONFIG_EXFAT_FS, CONFIG_F2FS_FS, CONFIG_EXT4_FS,
#                     CONFIG_BLK_DEV_INITRD, CONFIG_DEVTMPFS, CONFIG_RD_ZSTD; checked below)
#   installer.dtb     the daily DTB with the DWC3 in peripheral mode (dr_mode = "peripheral"), and the
#                     /chosen linux,initrd-start/-end placeholders the loader fills (as the store's DTBs)
# Size budget: kernel + initrd + DTB must stay below the loader's download buffer (0x04ff0000 = 80 MiB
# minus 64 KiB, docs/USB-FASTBOOT.md 3); the initrd alone below 80 MiB (xnu_boot.c OS_RD..EDIT_FDT).
# NOT BUILT YET (2026-11-06): the first build and the first boot on the phone are open items.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
U=${SUDO_USER:-$(id -un)}
UH=$(getent passwd "$U" | cut -d: -f6)
AV=${ALPINE_VERSION:-3.24.3}
AB=v${AV%.*}
MIRROR=${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}
CACHE=${CACHE:-$UH/distros/cache}
OUT=${1:-$UH/distros/out/installer-env}
BUSYBOX=${BUSYBOX:-/mnt/build/out/busybox-arm64}
KERNEL=${KERNEL:-$UH/distros/out/linux.image}
KCONFIG=${KCONFIG:-$UH/linux/build/.config}
DTB=${DTB:-$UH/distros/out/linux.dtb}
COMP=${COMP:-zstd}
PKGS="e2fsprogs exfatprogs f2fs-tools sgdisk dropbear util-linux coreutils"

[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 1; }
[ "$(uname -m)" = aarch64 ] || { echo "needs an arm64 host (the Lima VM)" >&2; exit 1; }
file "$BUSYBOX" | grep -q 'statically linked' || { echo "$BUSYBOX is not a static busybox" >&2; exit 1; }
for a in devmem udhcpd setsid; do "$BUSYBOX" --list | grep -qx $a || { echo "$BUSYBOX has no $a" >&2; exit 1; }; done
[ -s "$KERNEL" ] && [ -s "$DTB" ] || { echo "need $KERNEL and $DTB (tools/linux/build-kernel.sh)" >&2; exit 1; }
if [ -s "$KCONFIG" ]; then
	for c in USB_CONFIGFS_ECM USB_CONFIGFS_ACM EXFAT_FS F2FS_FS EXT4_FS BLK_DEV_INITRD DEVTMPFS SCSI_UFS_EXYNOS; do
		grep -q "^CONFIG_$c=y" "$KCONFIG" || { echo "the kernel lacks CONFIG_$c=y ($KCONFIG)" >&2; exit 1; }
	done
	[ "$COMP" != zstd ] || grep -q '^CONFIG_RD_ZSTD=y' "$KCONFIG" || { echo "no CONFIG_RD_ZSTD: COMP=gzip" >&2; exit 1; }
fi

mkdir -p "$CACHE" "$OUT"
TGZ=alpine-minirootfs-$AV-aarch64.tar.gz
[ -s "$CACHE/$TGZ" ] || { curl -fsSL -o "$CACHE/$TGZ.part" "$MIRROR/$AB/releases/aarch64/$TGZ"; mv "$CACHE/$TGZ.part" "$CACHE/$TGZ"; }
SHA=$(curl -fsSL "$MIRROR/$AB/releases/aarch64/$TGZ.sha256" | awk '{print $1}')
echo "$SHA  $CACHE/$TGZ" | sha256sum -c --quiet || { rm -f "$CACHE/$TGZ"; echo "bad $TGZ" >&2; exit 1; }

W=$(mktemp -d /var/tmp/sb-env.XXXX)
R=$W/r
cleanup() { for m in "$R/proc" "$R/dev"; do mountpoint -q "$m" && umount "$m"; done; rm -rf "$W"; }
trap cleanup EXIT
mkdir -p "$R"
tar -xzpf "$CACHE/$TGZ" -C "$R"
cp /etc/resolv.conf "$R/etc/"
mount -t proc proc "$R/proc"
mount --bind /dev "$R/dev"
chroot "$R" /sbin/apk add --no-cache $PKGS >/dev/null
umount "$R/proc" "$R/dev"
rm -rf "$R/var/cache/apk"/* "$R/etc/resolv.conf"
install -m755 "$BUSYBOX" "$R/bin/busybox.static"
for a in devmem udhcpd; do ln -sf busybox.static "$R/bin/$a"; done
install -m755 "$HERE/init" "$R/init"
for t in sb-byname getprop sb-reboot; do install -m755 "$HERE/$t" "$R/bin/$t"; done
mkdir -p "$R/root/.ssh" "$R/etc/dropbear" "$R/external_sd"
chmod 700 "$R/root" "$R/root/.ssh"
echo "Starbite installer environment, built $(date -u +%FT%TZ) from $(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo ?)" > "$R/etc/starbite-env"
mknod -m 600 "$R/dev/console" c 5 1 2>/dev/null || true
mknod -m 666 "$R/dev/null" c 1 3 2>/dev/null || true
case $COMP in zstd) Z="zstd -19 -q" ;; gzip) Z="gzip -9" ;; *) echo "COMP zstd|gzip" >&2; exit 1 ;; esac
(cd "$R" && find . -mindepth 1 | LC_ALL=C sort | cpio -o -H newc -R 0:0 --quiet) | $Z > "$OUT/installer.initrd"
cp "$KERNEL" "$OUT/installer.image"
# the DTB: DWC3 as a USB device (the store's linux.dtb may be host mode for rescue disks)
dtc -I dtb -O dts -o "$W/env.dts" "$DTB" 2>/dev/null
sed -i 's/dr_mode = "host"/dr_mode = "peripheral"/; s/dr_mode = "otg"/dr_mode = "peripheral"/' "$W/env.dts"
dtc -I dts -O dtb -o "$OUT/installer.dtb" "$W/env.dts" 2>/dev/null
grep -q 'linux,initrd-start' "$W/env.dts" || echo "WARNING: the DTB has no /chosen linux,initrd-* placeholders (the loader needs them)" >&2
total=$(( $(stat -c %s "$OUT/installer.image") + $(stat -c %s "$OUT/installer.initrd") + $(stat -c %s "$OUT/installer.dtb") + 6144 ))
[ $total -le $((0x04ff0000)) ] || { echo "kernel+initrd+dtb = $total bytes > the loader's 0x04ff0000 download buffer" >&2; exit 1; }
chown -R "$U": "$OUT"
(cd "$OUT" && sha256sum installer.image installer.initrd installer.dtb) > "$OUT/SHA256SUMS"
echo "$OUT: $(( total / 1048576 )) MiB of the 80 MiB download buffer"
