#!/usr/bin/env bash
# reintegrate.sh IMG NAME systemd|openrc: run sb-integrate.sh again inside an already built distro
# image, in place (no rebuild, no downloads, a few MB written). For fixes to the integration only.
# In the VM:
#   limactl shell starbite -- sudo bash /Users/dev/project-starbite/tools/linux/distros/reintegrate.sh \
#       ~/distros/ubuntu.img ubuntu systemd
# Then: check-root.sh IMG. IMG.sha256 is rewritten. The login PIN is the existing
# build/linux/NAME-login.txt (mk-payload.sh), so it does not change.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
U=${SUDO_USER:-$(id -un)}
UH=$(getent passwd "$U" | cut -d: -f6)
export P=${P:-/Users/dev/project-starbite}
IMG=${1:?reintegrate.sh IMG NAME systemd|openrc}
NAME=${2:?reintegrate.sh IMG NAME systemd|openrc}
INITSYS=${3:?reintegrate.sh IMG NAME systemd|openrc}
[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 1; }
[ -f "$IMG" ] || { echo "no $IMG" >&2; exit 1; }
[ -s "$P/build/linux/$NAME-login.txt" ] || { echo "no $P/build/linux/$NAME-login.txt: a new PIN would be made" >&2; exit 1; }
R=$(mktemp -d /mnt/sb-re.XXXX)
LOOP=
cleanup() {
	set +e
	for m in "$R/tmp/sb" "$R/dev/pts" "$R/dev" "$R/proc" "$R/sys" "$R/run"; do mountpoint -q "$m" && umount "$m"; done
	mountpoint -q "$R" && umount "$R"
	[ -n "$LOOP" ] && losetup -d "$LOOP"
	rmdir "$R"
}
trap cleanup EXIT
e2fsck -fy "$IMG" >/dev/null || [ $? -le 1 ]
LOOP=$(losetup -f --show "$IMG")
mount -o noatime "$LOOP" "$R"
mount --bind /dev "$R/dev"; mount --bind /dev/pts "$R/dev/pts"
mount -t proc proc "$R/proc"; mount -t sysfs sys "$R/sys"; mount -t tmpfs tmpfs "$R/run"
mkdir -p "$R/tmp/sb"
mount -t tmpfs tmpfs "$R/tmp/sb"                     # the payload never lands in the image
cp "$HERE/sb-integrate.sh" "$R/tmp/sb/"
UH=$UH sh "$HERE/mk-payload.sh" "$R/tmp/sb/payload" "$NAME" >/dev/null
keep=$(grep -v -e '^name=' -e '^init=' -e '^user=' -e '^integrated=' -e '^reintegrated=' "$R/etc/starbite/integrated" 2>/dev/null || true)
chroot "$R" /bin/sh /tmp/sb/sb-integrate.sh "$NAME" "$INITSYS" /tmp/sb/payload
{ [ -n "$keep" ] && echo "$keep"; echo "reintegrated=$(date -u +%FT%TZ) (reintegrate.sh)"; } >> "$R/etc/starbite/integrated"
for m in "$R/tmp/sb" "$R/dev/pts" "$R/dev" "$R/proc" "$R/sys" "$R/run"; do umount "$m"; done
rmdir "$R/tmp/sb" 2>/dev/null || true
fstrim "$R" >/dev/null 2>&1 || true                  # freed blocks become holes in IMG
umount "$R"
e2fsck -fy "$LOOP" >/dev/null || [ $? -le 1 ]
losetup -d "$LOOP"; LOOP=
chown "$U": "$IMG"
(cd "$(dirname "$IMG")" && sha256sum "$(basename "$IMG")") > "$IMG.sha256"
chown "$U": "$IMG.sha256"
echo "$IMG: re-integrated ($NAME, $INITSYS), sha256 $(cut -d' ' -f1 "$IMG.sha256")"
