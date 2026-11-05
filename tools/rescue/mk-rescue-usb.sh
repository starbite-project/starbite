#!/usr/bin/env bash
# Project Starbite: a rescue USB disk IMAGE FILE (docs/RESCUE-USB-HOST.md, tools/rescue/README.md).
# It never writes a real disk: OUT must be a new or regular file, never a block device.
#
#   limactl shell starbite -- bash /Users/dev/project-starbite/tools/rescue/mk-rescue-usb.sh \
#       [--fs fat32|exfat|ext4] [--size MIB] [--script FILE] OUT.img
#
# Layout: an MBR with one partition from 1 MiB to the end, holding
#   /starbite-rescue/rescue.sh      the script the rescue ramdisk runs (default: tools/rescue/usb/rescue.sh)
#   /starbite-rescue/rescue.sha256  sha256sum -c list of rescue.sh (the ramdisk refuses a mismatch)
#   /starbite-rescue/logs/          where the sample script saves its logs
#   /README.txt
# Filesystem and label (the ramdisk accepts both labels):
#   fat32 (default)  label SB-RESCUE        type 0x0c; macOS, Windows and Linux read and write it
#   exfat            label SB-RESCUE        type 0x07; the same, needs a kernel exfat or exfat-fuse
#   ext4             label STARBITE-RESCUE  type 0x83; Linux only
# FAT32 and exFAT labels hold 11 characters, so they cannot say STARBITE-RESCUE (15).
# exfat/fat32 need root for a loop mount of the partition image (sudo is used if needed); ext4
# is built with mke2fs -d, no root. Tools: sfdisk, mkfs.exfat (exfatprogs) / mkfs.vfat / mke2fs.
# Write the image to a stick yourself (macOS: diskutil list, then dd to the right /dev/rdiskN).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
FS=fat32 SIZE=64 SCRIPT=$HERE/usb/rescue.sh OUT=
while [ $# -gt 0 ]; do
	case $1 in
	--fs) FS=$2; shift 2 ;;
	--size) SIZE=$2; shift 2 ;;
	--script) SCRIPT=$2; shift 2 ;;
	-*) echo "unknown option $1" >&2; exit 2 ;;
	*) OUT=$1; shift ;;
	esac
done
[ -n "$OUT" ] || { echo "usage: $0 [--fs fat32|exfat|ext4] [--size MIB] [--script FILE] OUT.img" >&2; exit 2; }
if [ -e "$OUT" ] && [ ! -f "$OUT" ]; then
	echo "$OUT exists and is not a regular file: this tool only writes image files" >&2
	exit 1
fi
case $OUT in /dev/*) echo "refusing $OUT: this tool only writes image files" >&2; exit 1 ;; esac
case $FS in
exfat) LABEL=SB-RESCUE PTYPE=7 ;;
fat32) LABEL=SB-RESCUE PTYPE=c ;;
ext4) LABEL=STARBITE-RESCUE PTYPE=83 ;;
*) echo "--fs fat32|exfat|ext4" >&2; exit 2 ;;
esac
[ "$SIZE" -ge 40 ] || { echo "--size at least 40 (MiB)" >&2; exit 2; }
[ -f "$SCRIPT" ] || { echo "no $SCRIPT" >&2; exit 1; }
SUDO=; [ "$(id -u)" = 0 ] || SUDO=sudo

W=$(mktemp -d "${TMPDIR:-/var/tmp}/sb-rescue-usb.XXXX")
cleanup() {
	mountpoint -q "$W/mnt" 2>/dev/null && $SUDO umount "$W/mnt"
	rm -rf "$W"
}
trap cleanup EXIT

# the files
S=$W/stage
mkdir -p "$S/starbite-rescue/logs"
install -m755 "$SCRIPT" "$S/starbite-rescue/rescue.sh"
(cd "$S/starbite-rescue" && sha256sum rescue.sh > rescue.sha256)
cat > "$S/README.txt" <<EOF
Starbite rescue USB disk ($FS, label $LABEL), made $(date -u +%FT%TZ) by tools/rescue/mk-rescue-usb.sh.

On the phone: Starbite Recovery menu (hold Bixby after the Samsung splash) > Starbite Rescue mode,
then plug this disk in through a USB-C OTG adapter. The rescue ramdisk mounts it read-only, checks
starbite-rescue/rescue.sha256 and runs starbite-rescue/rescue.sh.

The sample script shows system information, saves it with the kernel log to
starbite-rescue/logs/, and offers: Volume Up = TWRP, Volume Down = Download mode, Power = back.
Editing rescue.sh? Update rescue.sha256 too (sha256sum rescue.sh > rescue.sha256), or delete it.
EOF

# the partition, then the disk
P=$W/part.img
truncate -s $(( (SIZE - 1) * 1048576 )) "$P"
case $FS in
ext4)
	mke2fs -q -t ext4 -L "$LABEL" -m 0 -E root_owner=0:0 -d "$S" "$P"
	e2fsck -fn "$P" >/dev/null
	;;
exfat | fat32)
	if [ $FS = exfat ]; then
		mkfs.exfat -L "$LABEL" "$P" >/dev/null
	else
		mkfs.vfat -F 32 -n "$LABEL" "$P" >/dev/null
	fi
	mkdir -p "$W/mnt"
	if [ $FS = exfat ] && ! grep -qw exfat /proc/filesystems && ! $SUDO modprobe exfat 2>/dev/null; then
		command -v mount.exfat-fuse >/dev/null ||
			{ echo "no exfat in this kernel and no mount.exfat-fuse: use --fs fat32" >&2; exit 1; }
		$SUDO mount.exfat-fuse "$P" "$W/mnt"
	else
		$SUDO mount -o loop "$P" "$W/mnt"
	fi
	$SUDO cp -r "$S/." "$W/mnt/"
	$SUDO sync
	$SUDO umount "$W/mnt"
	if [ $FS = exfat ]; then fsck.exfat -n "$P" >/dev/null; else fsck.vfat -n "$P" >/dev/null; fi
	;;
esac

rm -f "$OUT.part"
truncate -s $(( SIZE * 1048576 )) "$OUT.part"
printf 'label: dos\nstart=2048, type=%s\n' "$PTYPE" | sfdisk -q "$OUT.part"
dd if="$P" of="$OUT.part" bs=1M seek=1 conv=notrunc status=none
mv "$OUT.part" "$OUT"
(cd "$(dirname "$OUT")" && sha256sum "$(basename "$OUT")") > "$OUT.sha256"
echo "$OUT: ${SIZE} MiB, MBR + $FS labelled $LABEL, $(basename "$SCRIPT") as starbite-rescue/rescue.sh"
