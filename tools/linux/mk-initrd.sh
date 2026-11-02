#!/usr/bin/env bash
# The store's linux.initrd (docs/LINUX-REBUILD-PLAN.md 2.4): one initrd for every Linux entry.
# Built natively in the arm64 Lima VM, from the Alpine minirootfs (musl, small) plus:
#   e2fsprogs (+extra: resize2fs) exfatprogs losetup (--direct-io) dosfstools evtest
#   Ubuntu's static busybox (devmem udhcpd telnetd watchdog switch_root ...) as /bin/busybox and
#     /bin/busybox.static (copied into every distro for reboot-to)
#   tools/linux/init as /init, tools/linux/reboot-to, tools/linux/initrd/* (sb-tools)
#   the BCM4361 Wi-Fi firmware: only a fallback. brcmfmac is built in (=y) and the kernel carries
#     the firmware itself (CONFIG_EXTRA_FIRMWARE); this copy (~1 MiB) serves a kernel built
#     without it, since a built-in driver asks before any root is mounted
# Packed as newc cpio | zstd -19 (the kernel has RD_ZSTD; COMP=gzip for RD_GZIP). Budget 20 MiB,
# hard limit 80 MiB (xnu_boot.c: OS_RD 0xA8000000 .. EDIT_FDT 0xAD000000).
#
#   limactl shell starbite -- sudo bash /Users/dev/project-starbite/tools/linux/mk-initrd.sh [OUT]
#
# OUT defaults to ~/distros/out/linux.initrd in the VM (plus OUT.sha256). Writes ~25 MB of
# temporary files under /var/tmp, removed at the end; the Alpine tarball is cached (3 MB).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
U=${SUDO_USER:-$(id -un)}
UH=$(getent passwd "$U" | cut -d: -f6)
AV=${ALPINE_VERSION:-3.24.3}
AB=v${AV%.*}
ARCH=aarch64
MIRROR=${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}
CACHE=${CACHE:-$UH/distros/cache}
OUT=${1:-$UH/distros/out/linux.initrd}
BUSYBOX=${BUSYBOX:-/mnt/build/out/busybox-arm64}
FW=${FW:-$UH/linux/s9p/device/base/lib/firmware/brcm}
COMP=${COMP:-zstd}
PKGS="e2fsprogs e2fsprogs-extra exfatprogs losetup dosfstools evtest"

[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 1; }
[ "$(uname -m)" = aarch64 ] || { echo "needs an arm64 host (the Lima VM)" >&2; exit 1; }
file "$BUSYBOX" | grep -q 'statically linked' || { echo "$BUSYBOX is not a static busybox (fetch_busybox.py)" >&2; exit 1; }
"$BUSYBOX" --list | grep -qx devmem || { echo "$BUSYBOX has no devmem" >&2; exit 1; }
for f in brcmfmac4361-pcie.bin brcmfmac4361-pcie.txt brcmfmac4361-pcie.clm_blob; do
	[ -s "$FW/$f" ] || { echo "missing $FW/$f" >&2; exit 1; }
done

# ---- the Alpine minirootfs, checked against the release's sha256 (HTTPS) ----------------------
mkdir -p "$CACHE" "$(dirname "$OUT")"
TGZ=alpine-minirootfs-$AV-$ARCH.tar.gz
if [ ! -s "$CACHE/$TGZ" ]; then
	curl -fsSL -o "$CACHE/$TGZ.part" "$MIRROR/$AB/releases/$ARCH/$TGZ"
	mv "$CACHE/$TGZ.part" "$CACHE/$TGZ"
fi
SHA=$(curl -fsSL "$MIRROR/$AB/releases/$ARCH/$TGZ.sha256" | awk '{print $1}')
echo "$SHA  $CACHE/$TGZ" | sha256sum -c --quiet || { rm -f "$CACHE/$TGZ"; echo "bad $TGZ" >&2; exit 1; }

W=$(mktemp -d /var/tmp/sb-initrd.XXXX)
R=$W/r
cleanup() {
	for m in "$R/proc" "$R/dev"; do mountpoint -q "$m" && umount "$m"; done
	rm -rf "$W"
}
trap cleanup EXIT
mkdir -p "$R"
tar -xzpf "$CACHE/$TGZ" -C "$R"

# ---- packages (apk checks their signatures against the keys in the minirootfs) ---------------
printf '%s/%s/main\n%s/%s/community\n' "$MIRROR" "$AB" "$MIRROR" "$AB" > "$R/etc/apk/repositories"
cp /etc/resolv.conf "$R/etc/resolv.conf"
mount -t proc proc "$R/proc"
mount --bind /dev "$R/dev"
chroot "$R" /sbin/apk add --no-cache --no-progress $PKGS >/dev/null
chroot "$R" /sbin/apk info -v 2>/dev/null | sort > "$W/packages.txt"
umount "$R/proc" "$R/dev"

# ---- trim: no package manager, docs or headers in the initrd ---------------------------------
rm -rf "$R"/etc/apk "$R"/lib/apk "$R"/var/cache/apk "$R"/usr/share/apk "$R"/sbin/apk \
	"$R"/usr/share/man "$R"/usr/share/doc "$R"/usr/share/info "$R"/usr/include "$R"/usr/share/misc \
	"$R"/etc/resolv.conf "$R"/media "$R"/srv "$R"/opt "$R"/home
find "$R" \( -name '*.a' -o -name '*.la' \) -delete

# ---- busybox: Ubuntu's static build replaces Alpine's (devmem udhcpd telnetd are in it) -------
rm -f "$R"/lib/libapk.so*
install -m755 "$BUSYBOX" "$R/bin/busybox"
install -m755 "$BUSYBOX" "$R/bin/busybox.static"
"$BUSYBOX" --list > "$W/applets"
for l in $(find "$R" -type l -lname '*busybox'); do       # Alpine applets this busybox lacks
	grep -Fqx "${l##*/}" "$W/applets" || rm -f "$l"
done
for a in $("$BUSYBOX" --list); do
	case $a in busybox|linuxrc) continue ;; esac
	found=
	for d in bin sbin usr/bin usr/sbin; do
		{ [ -e "$R/$d/$a" ] || [ -L "$R/$d/$a" ]; } && { found=1; break; }
	done
	[ -n "$found" ] || ln -s busybox "$R/bin/$a"     # real tools (e2fsck, losetup ...) win
done

# ---- Starbite files ---------------------------------------------------------------------------
install -m755 "$HERE/init" "$R/init"
install -m755 "$HERE/reboot-to" "$R/bin/reboot-to"
ln -sf reboot-to "$R/bin/reboot-linux"
for f in "$HERE"/initrd/*; do install -m755 "$f" "$R/bin/${f##*/}"; done
install -d "$R/lib/firmware/brcm"
for f in brcmfmac4361-pcie.bin brcmfmac4361-pcie.txt brcmfmac4361-pcie.clm_blob; do
	install -m644 "$FW/$f" "$R/lib/firmware/brcm/$f"
done
mkdir -p "$R"/proc "$R"/sys "$R"/dev "$R"/run "$R"/tmp "$R"/sd "$R"/newroot
[ -e "$R/dev/console" ] || mknod -m 600 "$R/dev/console" c 5 1   # the kernel opens it before /init
[ -e "$R/dev/null" ] || mknod -m 666 "$R/dev/null" c 1 3
{
	echo "Starbite linux.initrd, built $(date -u +%FT%TZ) on $(hostname)"
	echo "alpine-minirootfs $AV ($SHA)"
	echo "busybox $("$BUSYBOX" | head -1)"
	cat "$W/packages.txt"
} > "$R/etc/starbite-initrd"

# ---- pack ---------------------------------------------------------------------------------------
case $COMP in
zstd) Z="zstd -19 -T0 -q" ;;
gzip) Z="gzip -9" ;;
*) echo "COMP is zstd or gzip" >&2; exit 1 ;;
esac
(cd "$R" && find . -mindepth 1 | LC_ALL=C sort | cpio -o -H newc -R 0:0 --quiet) | $Z > "$OUT.part"
mv "$OUT.part" "$OUT"
chown "$U": "$OUT"
size=$(stat -c %s "$OUT")
(cd "$(dirname "$OUT")" && sha256sum "$(basename "$OUT")") > "$OUT.sha256"
chown "$U": "$OUT.sha256"
echo "$OUT: $((size / 1024)) KiB ($COMP), unpacked $(du -sh "$R" | cut -f1)"
[ "$size" -le $((20 << 20)) ] || echo "WARNING: over the 20 MiB budget" >&2
[ "$size" -le $((80 << 20)) ] || { echo "over the 80 MiB hard limit" >&2; exit 1; }
