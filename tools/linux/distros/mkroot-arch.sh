#!/usr/bin/env bash
# Arch Linux ARM (aarch64) + Phosh root for the S9: /distros/arch.img on the card's exFAT (p1), ext4,
# label sb-arch (docs/LINUX-REBUILD-PLAN.md 3.5 "after these three"). Built natively in the arm64
# Lima VM into a sparse image on the Mac's APFS disk, through a writable virtiofs mount: never on the
# VM's own disk (a sparse file on the Mac that never shrinks) and never on the exFAT USB stick (no
# sparse files: every image is zero-filled first, which stalled an earlier run):
#
#   limactl shell starbite -- sudo bash /Users/dev/project-starbite/tools/linux/distros/mkroot-arch.sh
#
# 1. ArchLinuxARM-aarch64-latest.tar.gz from os.archlinuxarm.org, checked against its .md5 and its
#    .sig (Arch Linux ARM Build System key, $ALARM_KEY), extracted with bsdtar into the image.
# 2. In a chroot: pacman-key --init/--populate archlinuxarm; the distro kernel, its firmware and its
#    initramfs generator are removed (pacman -Rdd) and put in IgnorePkg; the tarball's "alarm" user
#    (password alarm), its root password and its DHCP-everything networkd files are removed.
# 3. pacman -Syu, then Phosh (extra) + NetworkManager, openssh, mesa, PipeWire, squeekboard, sudo.
# 4. sb-integrate.sh arch systemd: user "user" 10000, usb0 172.16.42.1 + DHCP for the Mac,
#    sshd keys made on the phone, GPU floor, AFBC env, scale 3, Wi-Fi firmware, starbite-session
#    (Phosh autologin on tty7 for "user"), watchdog off, no distro kernel.
# 5. e2fsck -f, resize2fs -M, file truncated to the filesystem: IMG + IMG.sha256.
#
# Disk: everything (download, image, temp files, pacman's package cache, which is the image's own
# and emptied after each step) lives in BUILD on the Mac; the VM gets only bsdtar and a gpg home in
# RAM. BUILD: /Users/dev/distros-build when the VM can write it (Lima mounts /Users/dev
# read-only by default), else the main checkout's git-ignored build/distros-build (the same APFS
# disk). Big steps wait (not fail) while the Mac has under MIN_FREE_GB (5) free.
# Login PIN: build/linux/arch-login.txt in the main checkout (git-ignored, made on the first run).
# Card: card-install.sh reads VM ~/distros/NAME.img; SRC=... or a copy/link there (see its header).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
U=${SUDO_USER:-$(id -un)}
UH=$(getent passwd "$U" | cut -d: -f6)
export P=${P:-/Users/dev/project-starbite}
NAME=arch
# the Mac's APFS disk, sparse and freed on delete (see the header)
if [ -z "${BUILD:-}" ]; then
	BUILD=/Users/dev/distros-build
	{ mkdir -p "$BUILD" && [ -w "$BUILD" ]; } 2>/dev/null || BUILD=$P/build/distros-build
fi
IMG=${1:-$BUILD/$NAME.img}
TMPD=${TMPD:-$BUILD/tmp-$NAME}
MIN_FREE_GB=${MIN_FREE_GB:-5}
SIZE=${SIZE:-5G}                                    # the image while building; shrunk at the end
BASE=${BASE:-http://os.archlinuxarm.org/os}
TARBALL=ArchLinuxARM-aarch64-latest.tar.gz
ALARM_KEY=68B3537F39A313B3E574D06777193F152BDBE6A6  # Arch Linux ARM Build System <builder@archlinuxarm.org>
# what the phone needs; the rest of the list is installed only when the repo has it
PKGS="phosh phoc squeekboard phosh-mobile-settings gnome-console networkmanager openssh mesa
	pipewire pipewire-pulse pipewire-alsa wireplumber sudo polkit wireless-regdb wlr-randr evtest
	cantarell-fonts ttf-dejavu xdg-desktop-portal-gtk vulkan-panfrost mesa-utils kmscube glmark2"
NEED="phosh phoc networkmanager openssh mesa pipewire wireplumber sudo"
# the kernel, its firmware, its initramfs generator and any boot loader: never on a Starbite root
NOKERNEL_RE='^(linux-aarch64.*|linux-firmware.*|mkinitcpio.*|uboot.*|grub|efibootmgr|firmware-raspberrypi|raspberrypi-bootloader.*)$'
IGNORE="linux-aarch64 linux-aarch64-headers linux-firmware linux-firmware-* mkinitcpio mkinitcpio-busybox uboot-* grub"

[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 1; }
[ "$(uname -m)" = aarch64 ] || { echo "needs an arm64 host (the Lima VM)" >&2; exit 1; }
case $IMG in /Volumes/*) echo "not on the exFAT stick: it has no sparse files ($IMG)" >&2; exit 1 ;; esac
mkdir -p "$BUILD" && [ -w "$BUILD" ] || { echo "cannot write $BUILD" >&2; exit 1; }
vm_free_gb() { df -BG --output=avail / | tail -1 | tr -dc 0-9; }
[ "$(vm_free_gb)" -ge 5 ] || { echo "only $(vm_free_gb) GB free in the VM" >&2; exit 1; }
# the Mac's free space (virtiofs reports the host filesystem); three builds share it: wait, never fail
# mac_wait WHAT [GB]: wait while the Mac has under MIN_FREE_GB + GB (what WHAT will write) free
mac_wait() {
	local f need=$(( MIN_FREE_GB + ${2:-0} ))
	while f=$(df -BG --output=avail "$BUILD" | tail -1 | tr -dc 0-9); [ "$f" -lt "$need" ]; do
		echo "the Mac has ${f} GB free (< $need): waiting before $1 ..."; sleep 60
	done
}
# Deleting a file on virtiofs only unlinks it: the guest's cached inode keeps it open on the Mac
# (com.apple.Virtualization) and its blocks stay allocated until the VM forgets it, so every rerun
# leaked a whole image. release FILE...: truncate to 0 first (APFS frees the blocks even while the
# file is held open), then unlink, then drop the guest's dentry/inode caches so the host closes it.
release() {
	local f
	for f in "$@"; do [ -f "$f" ] && truncate -s 0 "$f"; rm -f "$f"; done
	sync; echo 2 > /proc/sys/vm/drop_caches 2>/dev/null || true
}
# release_dir DIR: release every file in DIR, then the directory
release_dir() {
	[ -d "$1" ] || return 0
	find "$1" -type f -exec truncate -s 0 {} + 2>/dev/null || true
	rm -rf "$1"; release
}
# the image is sparse: blocks freed inside it go back to the Mac only when trimmed (loop -> hole)
trim() { sync; fstrim "$R" >/dev/null 2>&1 || true; }
if ! command -v bsdtar >/dev/null; then              # libarchive-tools: <1 MB; its .deb in RAM (/run)
	A=$(mktemp -d /run/sb-apt.XXXX); mkdir -p "$A/partial"
	DEBIAN_FRONTEND=noninteractive apt-get -q -y -o Dir::Cache::archives="$A" --no-install-recommends install libarchive-tools
	rm -rf "$A"
fi
mkdir -p "$TMPD"
export TMPDIR=$TMPD

R=/mnt/sb-$NAME
LOOP=
STARTED=0                                            # this run made a new $IMG
DONE=0
in_root() { chroot "$R" /usr/bin/env LANG=C.UTF-8 TMPDIR=/tmp "$@"; }
# anything still running inside the root (pacman's gpg-agent) keeps it busy: unmount would fail
kill_root_procs() {
	local p r
	for p in /proc/[0-9]*; do
		r=$(readlink "$p/root" 2>/dev/null) || continue
		case $r in "$R"|"$R"/*) kill "${p#/proc/}" 2>/dev/null ;; esac
	done
	sleep 1
}
cleanup() {
	set +e
	[ -x "$R/usr/bin/gpgconf" ] && chroot "$R" gpgconf --homedir /etc/pacman.d/gnupg --kill all 2>/dev/null
	kill_root_procs
	for m in "$R/var/cache/pacman/pkg" "$R/tmp/sb" "$R/tmp" "$R/dev/pts" "$R/dev" "$R/proc" "$R/sys" "$R/run"; do mountpoint -q "$m" && umount "$m"; done
	mountpoint -q "$R" && umount "$R"
	[ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null
	# a failed run: give the half-built image's blocks back now (the next run starts it afresh;
	# the checked tarball stays in TMPD for reuse)
	[ "$STARTED" = 1 ] && [ "$DONE" != 1 ] && { echo "failed: releasing the half-built $IMG" >&2; release "$IMG" "$IMG.sha256"; }
	return 0
}
trap cleanup EXIT
mkdir -p "$R"

# ---- 1. the tarball: md5 + signature -----------------------------------------------------------
T=$TMPD/$TARBALL
mac_wait "the download" 1
if [ ! -s "$T" ] || [ "${REFETCH:-0}" = 1 ]; then
	echo "downloading $BASE/$TARBALL ..."
	release "$T" "$T.md5" "$T.sig"
	curl -fL --retry 3 -o "$T.part" "$BASE/$TARBALL" || { release "$T.part"; echo "download failed" >&2; exit 1; }
	mv "$T.part" "$T"
fi
# (virtiofs: a file just written can take a moment to show up; the small ones go via RAM)
seen() { local i=0; while [ ! -s "$1" ] && [ $i -lt 30 ]; do sleep 1; i=$((i + 1)); done; [ -s "$1" ]; }
seen "$T" || { echo "no $T" >&2; exit 1; }
TAR_MD5=$(curl -fsSL --retry 3 "$BASE/$TARBALL.md5" | awk '{print $1}')
G=$(mktemp -d /run/sb-arch-gpg.XXXX)                 # tmpfs (RAM): gpg sockets do not work on virtiofs
trap 'gpgconf --homedir "$G" --kill all 2>/dev/null; rm -rf "$G"; cleanup' EXIT
curl -fsSL --retry 3 -o "$G/tarball.sig" "$BASE/$TARBALL.sig"
got=$(md5sum "$T" | awk '{print $1}')
[ -n "$TAR_MD5" ] && [ "$got" = "$TAR_MD5" ] || { echo "md5 $got, expected '$TAR_MD5' (REFETCH=1 to download again)" >&2; exit 1; }
echo "$TAR_MD5  $TARBALL" > "$T.md5"
gpg --homedir "$G" -q --keyserver hkps://keyserver.ubuntu.com --recv-keys "$ALARM_KEY" ||
	curl -fsSL "https://keyserver.ubuntu.com/pks/lookup?op=get&search=0x$ALARM_KEY" | gpg --homedir "$G" -q --import
gpg --homedir "$G" --status-fd 1 --verify "$G/tarball.sig" "$T" 2>/dev/null | grep -q "VALIDSIG $ALARM_KEY" ||
	{ echo "signature of $TARBALL is not by $ALARM_KEY" >&2; exit 1; }
cp "$G/tarball.sig" "$T.sig"
echo "$TARBALL: md5 $TAR_MD5 ok, signed by $ALARM_KEY"

# ---- the image: sparse on the Mac, 5 GiB while building -----------------------------------------
mac_wait "the image" 2
release "$IMG" "$IMG.sha256"
STARTED=1
truncate -s "$SIZE" "$IMG"
mkfs_img() { mkfs.ext4 -q -F -E nodiscard -O ^orphan_file,^metadata_csum_seed -L "sb-$NAME" -m 0 "$IMG"; }
mkfs_img || { sleep 3; mkfs_img; }                   # virtiofs: the new file can lag a moment
LOOP=$(losetup -f --show "$IMG")
mount -o noatime "$LOOP" "$R"
echo "extracting $TARBALL ..."
bsdtar -xpf "$T" -C "$R"
mount --bind /dev "$R/dev"; mount --bind /dev/pts "$R/dev/pts"
mount -t proc proc "$R/proc"; mount -t sysfs sys "$R/sys"; mount -t tmpfs tmpfs "$R/run"
mount -t tmpfs -o size=1G tmpfs "$R/tmp"             # temp files in RAM, never the VM's disk
# pacman's package cache in RAM too: downloads never reach the image (the Mac's disk) or the VM's
mkdir -p "$R/var/cache/pacman/pkg"
mount -t tmpfs -o size=2G tmpfs "$R/var/cache/pacman/pkg"
rm -f "$R/etc/resolv.conf"; cp -L /etc/resolv.conf "$R/etc/resolv.conf"

# ---- 2. keys, no kernel, no tarball accounts ---------------------------------------------------
in_root pacman-key --init
in_root pacman-key --populate archlinuxarm
sed -i "s/^#\?IgnorePkg *=.*/IgnorePkg = $IGNORE/" "$R/etc/pacman.conf"
grep -q '^IgnorePkg = linux-aarch64' "$R/etc/pacman.conf" || { echo "IgnorePkg not set" >&2; exit 1; }
sed -i 's/^#\?ParallelDownloads.*/ParallelDownloads = 5/' "$R/etc/pacman.conf"
rmpk=$(in_root pacman -Qq | grep -E "$NOKERNEL_RE" || true)
if [ -n "$rmpk" ]; then
	echo "removing: $(echo $rmpk)"
	in_root pacman -Rdd --noconfirm $rmpk
fi
rm -rf "$R"/boot/* "$R"/usr/lib/modules/* "$R"/etc/mkinitcpio.* 2>/dev/null || true
# the tarball's login: alarm/alarm and root/root (sb-integrate locks root)
if in_root id alarm >/dev/null 2>&1; then in_root userdel -r alarm 2>/dev/null || in_root userdel alarm; fi
rm -rf "$R/home/alarm"
# the tarball's networkd files run DHCP on every en*/eth*: NetworkManager owns everything but usb0
rm -f "$R"/etc/systemd/network/*.network

# ---- 3. packages ------------------------------------------------------------------------------
trim
mac_wait "the packages" 1
in_root pacman -Syu --noconfirm
in_root pacman -Scc --noconfirm >/dev/null
trim
avail=()
for p in $PKGS; do
	if in_root pacman -Si "$p" >/dev/null 2>&1 || in_root pacman -Sg "$p" >/dev/null 2>&1; then avail+=("$p")
	else
		case " $(echo $NEED) " in *" $p "*) echo "$p is not packaged for aarch64: stopping (see the header)" >&2; exit 1 ;; esac
		echo "not in the repos: $p (skipped)"
	fi
done
echo "installing: ${avail[*]}"
mac_wait "installing" 3                              # ~2.3 GB installed
in_root pacman -S --needed --noconfirm "${avail[@]}"
in_root pacman -Scc --noconfirm >/dev/null
trim
left=$(in_root pacman -Qq | grep -E "$NOKERNEL_RE" || true)
[ -z "$left" ] || { echo "a kernel/boot package came back: $left" >&2; exit 1; }
[ -x "$R/usr/bin/phosh-session" ] || { echo "no /usr/bin/phosh-session after installing phosh" >&2; exit 1; }

# ---- 4. Starbite integration (shared with every distro) -----------------------------------------
mkdir -p "$R/tmp/sb"
mount -t tmpfs tmpfs "$R/tmp/sb"                     # the payload (PIN) never lands in the image
cp "$HERE/sb-integrate.sh" "$R/tmp/sb/"
UH=$UH sh "$HERE/mk-payload.sh" "$R/tmp/sb/payload" $NAME
chroot "$R" /bin/sh /tmp/sb/sb-integrate.sh $NAME systemd /tmp/sb/payload
umount "$R/tmp/sb"
echo phosh > "$R/etc/starbite/session"
{
	echo "source=$BASE/$TARBALL md5=$TAR_MD5 sig=$ALARM_KEY"
	echo "sessions installed: phosh"
} >> "$R/etc/starbite/integrated"

# ---- Arch specifics ------------------------------------------------------------------------------
# DNS: systemd-resolved's stub (NetworkManager feeds it); time from the initrd, NTP later
rm -f "$R/etc/resolv.conf"
ln -s ../run/systemd/resolve/stub-resolv.conf "$R/etc/resolv.conf"
in_root systemctl enable systemd-resolved.service >/dev/null 2>&1 || true
# the tarball enables things a phone root does not want
in_root systemctl disable haveged.service >/dev/null 2>&1 || true
# nothing of the build host stays (sb-integrate removed the ssh host keys; sb-ssh-keys makes the phone's)
in_root gpgconf --homedir /etc/pacman.d/gnupg --kill all 2>/dev/null || true
rm -f "$R"/etc/ssh/ssh_host_* "$R"/etc/pacman.d/gnupg/S.* "$R"/root/.bash_history
rm -rf "$R"/var/tmp/* "$R"/var/log/pacman.log "$R"/var/log/journal/* "$R"/var/cache/pacman/pkg/*

# ---- 5. unmount, check, shrink --------------------------------------------------------------------
kill_root_procs
for m in "$R/var/cache/pacman/pkg" "$R/tmp" "$R/dev/pts" "$R/dev" "$R/proc" "$R/sys" "$R/run"; do umount "$m"; done
rm -rf "$R"/tmp/*
trim
used=$(df -BM --output=used "$R" | tail -1 | tr -dc 0-9)
umount "$R"
e2fsck -fy "$LOOP" >/dev/null || [ $? -le 1 ]
resize2fs -M "$LOOP" >/dev/null 2>&1
e2fsck -fy "$LOOP" >/dev/null || [ $? -le 1 ]
bs=$(dumpe2fs -h "$LOOP" 2>/dev/null | awk -F: '/^Block size/ {print $2+0}')
blocks=$(dumpe2fs -h "$LOOP" 2>/dev/null | awk -F: '/^Block count/ {print $2+0}')
losetup -d "$LOOP"; LOOP=
truncate -s $(( blocks * bs )) "$IMG"
(cd "$(dirname "$IMG")" && sha256sum "$(basename "$IMG")") > "$IMG.sha256"
DONE=1
[ "${KEEP_TMP:-0}" = 1 ] || release_dir "$TMPD"      # the tarball and temp files: done with them
echo "$IMG: $(( blocks * bs / 1048576 )) MiB ext4 (sb-$NAME), ${used} MiB of files"
echo "manifest: $NAME $(( blocks * bs / 1048576 ))M $BASE/$TARBALL $(cut -d' ' -f1 "$IMG.sha256") $(date -I) 7.1.0-starbite"
