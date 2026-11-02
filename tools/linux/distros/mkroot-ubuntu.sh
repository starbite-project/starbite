#!/usr/bin/env bash
# Ubuntu 26.04 LTS ("resolute") root for the S9: /distros/ubuntu.img on the card's exFAT (p1),
# ext4, label sb-ubuntu. The MAIN Starbite distro. Built natively in the arm64 Lima VM:
#
#   limactl shell starbite -- sudo bash /Users/dev/project-starbite/tools/linux/distros/mkroot-ubuntu.sh
#
# Input: ~/distros/ubuntu-26.04-root.tar (mmdebstrap of phosh phoc squeekboard greetd, GNOME apps,
# mesa with panfrost, NetworkManager, openssh, glmark2, kmscube; see NOTES.md). On top:
#   FLAVOURS (default "gnome lomiri"): the same image also carries GNOME (ubuntu-session) and Lomiri
#   (universe), picked per boot by sb.session= in the catalog entry (sb-integrate.sh sb-session);
#   FLAVOURS="" builds Phosh only.
#   sb-integrate.sh ubuntu systemd: user "user" 10000, usb0, watchdog, GPU floor, scale 3, no kernel.
# Output: IMG (default ~/distros/ubuntu.img in the VM), shrunk to its data plus SLACK free, and
# IMG.sha256. card-install.sh copies it to the card and grows it there. Login PIN:
# build/linux/ubuntu-login.txt in the main checkout (git-ignored, made on the first run).
#
# Disk: the VM's disk is a sparse file on the Mac and only grows. The image is rebuilt IN PLACE over
# an existing IMG (mkfs without discard), apt's downloads are deleted inside it, the loop mount is
# trimmed before the shrink and the VM's / is trimmed at the end, so the Mac gets the space back.
# It refuses to start with less than NEED_GB free in the VM.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
U=${SUDO_USER:-$(id -un)}
UH=$(getent passwd "$U" | cut -d: -f6)
export P=${P:-/Users/dev/project-starbite}
TAR=${TAR:-$UH/distros/ubuntu-26.04-root.tar}
IMG=${1:-$UH/distros/ubuntu.img}
SIZE=${SIZE:-8G}                                    # while building; shrunk at the end
SLACK_MB=${SLACK_MB:-768}                           # free space left in the shrunk image
NEED_GB=${NEED_GB:-10}
FLAVOURS=${FLAVOURS-gnome lomiri}
SUITE=resolute
MIRROR=${MIRROR:-http://ports.ubuntu.com/ubuntu-ports}
NAME=ubuntu

[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 1; }
[ "$(uname -m)" = aarch64 ] || { echo "needs an arm64 host (the Lima VM)" >&2; exit 1; }
[ -s "$TAR" ] || { echo "no $TAR (mmdebstrap, NOTES.md)" >&2; exit 1; }
free_gb=$(df -BG --output=avail "$(dirname "$IMG")" | tail -1 | tr -dc 0-9)
[ "$free_gb" -ge "$NEED_GB" ] || { echo "only ${free_gb} GB free in the VM, need $NEED_GB" >&2; exit 1; }

R=/mnt/sb-$NAME
cleanup() {
	set +e
	for m in "$R/tmp/sb" "$R/dev/pts" "$R/dev" "$R/proc" "$R/sys" "$R/run"; do mountpoint -q "$m" && umount "$m"; done
	mountpoint -q "$R" && umount "$R"
	[ -n "${LOOP:-}" ] && losetup -d "$LOOP" 2>/dev/null
}
trap cleanup EXIT
mkdir -p "$R"

# ---- the image: in place over the old one when there is one -----------------------------------
[ -e "$IMG" ] || : > "$IMG"
truncate -s "$SIZE" "$IMG"
mkfs.ext4 -q -F -E nodiscard -O ^orphan_file,^metadata_csum_seed -L sb-$NAME -m 0 "$IMG"
LOOP=$(losetup -f --show "$IMG")
mount -o noatime "$LOOP" "$R"
echo "extracting $(basename "$TAR") ..."
tar -xpf "$TAR" -C "$R" --numeric-owner --xattrs --xattrs-include='*'

mount --bind /dev "$R/dev"; mount --bind /dev/pts "$R/dev/pts"
mount -t proc proc "$R/proc"; mount -t sysfs sys "$R/sys"; mount -t tmpfs tmpfs "$R/run"
rm -f "$R/etc/resolv.conf"; cp /etc/resolv.conf "$R/etc/resolv.conf"
printf '#!/bin/sh\nexit 101\n' > "$R/usr/sbin/policy-rc.d"; chmod 755 "$R/usr/sbin/policy-rc.d"

# ---- apt: main + universe, updates, security; Starbite's no-kernel pin before any install ------
rm -f "$R/etc/apt/sources.list"
cat > "$R/etc/apt/sources.list.d/ubuntu.sources" <<EOF
Types: deb
URIs: $MIRROR
Suites: $SUITE $SUITE-updates $SUITE-security
Components: main universe
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
EOF
mkdir -p "$R/etc/apt/preferences.d"
# (the pin is also written by sb-integrate; written here first so the flavour install obeys it)
sed -n '/^Package: linux-image/,/^Pin-Priority/p' "$HERE/sb-integrate.sh" > "$R/etc/apt/preferences.d/90-starbite-no-kernel"

PKGS="systemd-resolved wlr-randr evtest"
for f in $FLAVOURS; do
	case $f in
	gnome) PKGS="$PKGS ubuntu-session gnome-shell-extension-ubuntu-dock gnome-control-center" ;;
	lomiri) PKGS="$PKGS lomiri lomiri-desktop-session lomiri-system-settings lomiri-terminal-app" ;;
	*) echo "unknown flavour $f (gnome lomiri)" >&2; exit 1 ;;
	esac
done
in_root() { chroot "$R" /usr/bin/env DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8 "$@"; }
in_root apt-get -q update
avail=()
for p in $PKGS; do
	if in_root apt-cache show "$p" >/dev/null 2>&1; then avail+=("$p"); else echo "not in $SUITE: $p (skipped)"; fi
done
echo "installing: ${avail[*]}"
est=$(in_root apt-get --no-install-recommends install --assume-no "${avail[@]}" 2>&1 | grep -E 'After this operation|Need to get' || true)
echo "$est"
mb=$(echo "$est" | awk '/After this operation/ {v=$4; gsub(",","",v); u=$5; if (u ~ /^GB/) v*=1000; if (u ~ /^kB/) v/=1000; print int(v)}')
[ "${mb:-0}" -le "${MAX_ADD_MB:-2500}" ] || { echo "flavours add ${mb} MB > MAX_ADD_MB=${MAX_ADD_MB:-2500}: stopping (FLAVOURS= to drop some)" >&2; exit 1; }
in_root apt-get -q -y --no-install-recommends install "${avail[@]}"
in_root apt-get -q -y clean
rm -rf "$R"/var/lib/apt/lists/* "$R"/var/cache/apt/*.bin

# ---- Starbite integration (shared with every distro) ------------------------------------------
mkdir -p "$R/tmp/sb"
cp "$HERE/sb-integrate.sh" "$R/tmp/sb/"
UH=$UH sh "$HERE/mk-payload.sh" "$R/tmp/sb/payload" $NAME
chroot "$R" /bin/sh /tmp/sb/sb-integrate.sh $NAME systemd /tmp/sb/payload
rm -rf "$R/tmp/sb"
echo phosh > "$R/etc/starbite/session"               # the default when sb.session= is absent
installed=$(in_root dpkg-query -W -f='${Package}\n' gnome-shell lomiri phosh 2>/dev/null | tr '\n' ' ')
echo "sessions installed: $installed" | tee -a "$R/etc/starbite/integrated"

# ---- Ubuntu specifics ----------------------------------------------------------------------------
# DNS: systemd-resolved's stub when installed, else a plain file NetworkManager writes
rm -f "$R/etc/resolv.conf"
if [ -e "$R/usr/lib/systemd/systemd-resolved" ]; then
	ln -s ../run/systemd/resolve/stub-resolv.conf "$R/etc/resolv.conf"
	chroot "$R" systemctl enable systemd-resolved.service >/dev/null 2>&1 || true
else
	: > "$R/etc/resolv.conf"
fi
# netplan: NetworkManager renders everything but usb0 (networkd: sb-integrate's 10-usb0.network)
if [ -d "$R/etc/netplan" ]; then
	printf 'network:\n  version: 2\n  renderer: NetworkManager\n' > "$R/etc/netplan/01-network-manager-all.yaml"
	chmod 600 "$R/etc/netplan/01-network-manager-all.yaml"
fi
rm -f "$R/usr/sbin/policy-rc.d"
# nothing of the build host stays
rm -f "$R/etc/ssh/ssh_host_"*                        # sshd makes its own keys on the first boot
if [ -e "$R/usr/lib/systemd/system/ssh.service" ] && [ ! -e "$R/etc/systemd/system/ssh.service.d/10-keys.conf" ]; then
	mkdir -p "$R/etc/systemd/system/ssh.service.d"
	printf '[Service]\nExecStartPre=-/usr/bin/ssh-keygen -A\n' > "$R/etc/systemd/system/ssh.service.d/10-keys.conf"
fi
rm -rf "$R"/tmp/* "$R"/var/tmp/* "$R"/var/log/*.log "$R"/var/log/apt/*

# ---- unmount, check, trim, shrink ---------------------------------------------------------------
for m in "$R/dev/pts" "$R/dev" "$R/proc" "$R/sys" "$R/run"; do umount "$m"; done
used=$(df -BM --output=used "$R" | tail -1 | tr -dc 0-9)
fstrim "$R" || true                                 # free blocks become holes in IMG
umount "$R"
e2fsck -fy "$LOOP" >/dev/null || [ $? -le 1 ]
resize2fs -M "$LOOP" >/dev/null 2>&1
bs=$(dumpe2fs -h "$LOOP" 2>/dev/null | awk -F: '/^Block size/ {print $2+0}')
blocks=$(dumpe2fs -h "$LOOP" 2>/dev/null | awk -F: '/^Block count/ {print $2+0}')
target=$(( blocks + SLACK_MB * 1024 * 1024 / bs ))
resize2fs "$LOOP" "$target" >/dev/null 2>&1
e2fsck -fy "$LOOP" >/dev/null || [ $? -le 1 ]
losetup -d "$LOOP"; LOOP=
truncate -s $(( target * bs )) "$IMG"
chown "$U": "$IMG"
(cd "$(dirname "$IMG")" && sha256sum "$(basename "$IMG")") > "$IMG.sha256"
chown "$U": "$IMG.sha256"
fstrim / >/dev/null 2>&1 || true                    # hand the VM's freed blocks back to the Mac
echo "$IMG: $(( target * bs / 1048576 )) MiB ext4 (sb-$NAME), ${used} MiB of files, ${SLACK_MB} MiB free"
echo "manifest: $NAME $(( target * bs / 1048576 ))M ubuntu-$SUITE $(cut -d' ' -f1 "$IMG.sha256") $(date -I) 7.1.0-starbite"
