#!/usr/bin/env bash
# The Starbite Rescue ramdisk (tools/rescue/README.md): a static busybox, tools/rescue/init and
# /bin/sb-reboot (tools/rescue/sb-reboot: VBUS off before every reboot), nothing else (about
# 1 MiB gzip). Pair it with rescue.dtb (USB host, docs/RESCUE-USB-HOST.md). Built in the arm64 Lima VM like tools/linux/mk-initrd.sh:
#
#   limactl shell starbite -- sudo bash /Users/dev/project-starbite/tools/rescue/mk-rescue-initrd.sh [OUT]
#
# OUT defaults to ~/distros/out/rescue.initrd in the VM (plus OUT.sha256). Then either put it in
# the OS store as "rescue.initrd" (the loader pairs it with rescue.image|linux.image and
# rescue.dtb|linux.dtb) or bundle it into BOOT with tools/rescue/bundle-rescue.py.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
U=${SUDO_USER:-$(id -un)}
UH=$(getent passwd "$U" | cut -d: -f6)
OUT=${1:-$UH/distros/out/rescue.initrd}
BUSYBOX=${BUSYBOX:-/mnt/build/out/busybox-arm64}

[ "$(id -u)" = 0 ] || { echo "run as root (sudo): the cpio needs device nodes" >&2; exit 1; }
file "$BUSYBOX" | grep -q 'statically linked' || { echo "$BUSYBOX is not a static busybox" >&2; exit 1; }
for a in devmem blockdev sha256sum setsid mount dmesg; do
	"$BUSYBOX" --list | grep -qx "$a" || { echo "$BUSYBOX has no $a" >&2; exit 1; }
done
# the init finds the disk by label with blkid, or findfs (Ubuntu's busybox-static has no blkid)
"$BUSYBOX" --list | grep -qxE 'blkid|findfs' || { echo "$BUSYBOX has neither blkid nor findfs" >&2; exit 1; }

W=$(mktemp -d /var/tmp/sb-rescue.XXXX)
trap 'rm -rf "$W"' EXIT
R=$W/r
mkdir -p "$R"/bin "$R"/sbin "$R"/proc "$R"/sys "$R"/dev "$R"/run "$R"/tmp "$R"/rescue "$R"/etc
install -m755 "$BUSYBOX" "$R/bin/busybox"
for a in $("$BUSYBOX" --list); do
	case $a in busybox|linuxrc|init) continue ;; esac
	ln -s busybox "$R/bin/$a"
done
install -m755 "$HERE/init" "$R/init"
install -m755 "$HERE/sb-reboot" "$R/bin/sb-reboot"
mknod -m 600 "$R/dev/console" c 5 1
mknod -m 666 "$R/dev/null" c 1 3
touch "$R/.sb-initrd"                              # tools/linux/reboot-to: reboot from the ramdisk
echo "Starbite rescue.initrd, built $(date -u +%FT%TZ) on $(hostname)" > "$R/etc/starbite-rescue"
mkdir -p "$(dirname "$OUT")"
(cd "$R" && find . -mindepth 1 | LC_ALL=C sort | cpio -o -H newc -R 0:0 --quiet) | gzip -9 > "$OUT.part"
mv "$OUT.part" "$OUT"
chown "$U": "$OUT"
(cd "$(dirname "$OUT")" && sha256sum "$(basename "$OUT")") > "$OUT.sha256"
chown "$U": "$OUT.sha256"
echo "$OUT: $(( $(stat -c %s "$OUT") / 1024 )) KiB"
