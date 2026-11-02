#!/usr/bin/env bash
# Kali Linux (kali-rolling, arm64) root for the S9: /distros/kali.img on the card's exFAT (p1),
# ext4, label sb-kali, Phosh (as on Kali NetHunter Pro) + a phone-sized Kali tool set. Built
# natively in the arm64 Lima VM, into a sparse file on the Mac's APFS disk:
#
#   limactl shell starbite -- sudo bash /Users/dev/project-starbite/tools/linux/distros/mkroot-kali.sh
#
# Output: IMG (default OUT/kali.img), shrunk with resize2fs -M (the phone grows it: card-install.sh
# image kali [GROW]), plus IMG.sha256. OUT: /Users/dev/distros-build when the VM can write it
# (it mounts ~ read-only unless lima.yaml says otherwise), else the main checkout's git-ignored
# build/distros-build, which is on the same APFS disk. Never the VM's own disk (a sparse file on the
# Mac that never shrinks) and never the exFAT stick (no sparse files: every image is zero-filled).
# Login PIN for "user": build/linux/kali-login.txt in the main checkout (git-ignored, made on the first run).
#
# What goes in (docs/LINUX-REBUILD-PLAN.md 3.4/3.5):
#   mmdebstrap minbase of kali-rolling, signed by the Kali archive key fetched over HTTPS and checked
#   against KALI_FPR (Kali's 2025 archive signing key), then kali-archive-keyring from the archive
#   kali-linux-core, NetworkManager + wpasupplicant, openssh-server, mesa (panfrost GL/GLES, panvk
#   with VULKAN=1), kmscube/glmark2, phosh-core + phosh-mobile-settings, Console, Settings, Firefox
#   ESR (mobile config), kali-menu
#   TOOLS: kali-tools-top10 without its metapackage, without aircrack-ng (the S9's brcmfmac has no
#   monitor mode or injection, and the Starbite kernel has no mac80211 or USB Wi-Fi drivers, so no
#   adapter would give it one either; TOOLS="... aircrack-ng" adds it back for offline cracking),
#   without burpsuite (0.6 GB of Java) and with tshark instead of the Qt wireshark (0.3 GB): with
#   metasploit-framework (1.8 GB) that is what fits in 5 GiB. Metasploit goes in last, its
#   dependencies first and then the package, so apt's downloads never need room all at once.
#   sb-integrate.sh kali systemd: user "user" 10000, usb0 + DHCP for the Mac, sshd + host keys made
#   on the phone, Wi-Fi firmware, GPU floor, AFBC env, scale 3, starbite-session (Phosh autologin),
#   no kernel/bootloader/initramfs (apt pin -1 written BEFORE anything is installed)
#   NODOC=1: dpkg drops /usr/share/doc (copyrights kept), man/info, non-English locales and GNOME
#   help; this stays in the image (/etc/dpkg/dpkg.cfg.d/99mmdebstrap) for installs on the phone.
#
# Disk: the root is written into the loop-mounted image, apt's downloads into a second sparse ext4
# (TMPB/apt-archives.img, deleted at the end; installed in stages with apt-get clean and fstrim
# between, so they never need room at once). Stage sizes come from simulation only. mmdebstrap's TMPDIR is a
# small tmpfs (RAM: a fresh file on virtiofs can be missing for a moment, which breaks gpg). The VM's
# disk must not grow by more than VM_MAX_MB; checked between stages. Before each big step the build
# waits (never fails) while the Mac has less than MAC_MIN_GB free (several builds share it).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
U=${SUDO_USER:-$(id -un)}
UH=$(getent passwd "$U" | cut -d: -f6)
export P=${P:-/Users/dev/project-starbite}
NAME=kali
if [ -z "${OUT:-}" ]; then
	OUT=/Users/dev/distros-build
	mkdir -p "$OUT" 2>/dev/null && [ -w "$OUT" ] || OUT=$P/build/distros-build
fi
IMG=${1:-$OUT/$NAME.img}
TMPB=${TMPB:-$OUT/tmp-$NAME}                        # the keyring download
SIZE=${SIZE:-5G}                                    # the build's ceiling; shrunk with -M at the end
SUITE=kali-rolling
MIRROR=${MIRROR:-http://http.kali.org/kali}         # InRelease is checked against the key below
KEY_URL=https://archive.kali.org/archive-keyring.gpg
KALI_FPR=${KALI_FPR:-827C8569F2518CC677FECA1AED65462EC8D5E4C5}   # "Kali Linux Archive Automatic Signing Key (2025)"
VM_MAX_MB=${VM_MAX_MB:-300}
MAC_MIN_GB=${MAC_MIN_GB:-5}
NODOC=${NODOC:-1}
VULKAN=${VULKAN:-1}
CORE="kali-linux-core network-manager wpasupplicant wireless-regdb iw openssh-server sudo polkitd
 dbus-user-session libpam-systemd systemd-timesyncd less
 libgl1-mesa-dri libegl-mesa0 libgles2 libgbm1 mesa-utils kmscube glmark2-es2-drm glmark2-es2-wayland
 wlr-randr evtest"
[ "$VULKAN" = 1 ] && CORE="$CORE mesa-vulkan-drivers"
UI=${UI:-"phosh-core phosh-mobile-settings gnome-console gnome-control-center firefox-esr
 firefox-esr-mobile-config wireplumber xdg-desktop-portal-gtk kali-menu"}
TOOLS=${TOOLS:-"hydra john netexec nmap responder sqlmap tshark"}
MSF=${MSF:-1}                                       # metasploit-framework: 1.8 GB, its own two stages

[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 1; }
[ "$(uname -m)" = aarch64 ] || { echo "needs an arm64 host (the Lima VM)" >&2; exit 1; }
command -v mmdebstrap >/dev/null || { echo "no mmdebstrap" >&2; exit 1; }
mkdir -p "$OUT"
case $(stat -f -c %T "$OUT") in
	virtiofs|fuseblk|fuse*|nfs*|9p|v9fs) ;;           # the Mac's disk through Lima
	*) echo "$OUT is not a Mac mount ($(stat -f -c %T "$OUT")): refusing to build on the VM's disk" >&2; exit 1 ;;
esac
case $OUT in /Volumes/*) echo "$OUT: not on the exFAT stick" >&2; exit 1 ;; esac
mac_wait() {                                        # mac_wait STEP: the Mac's free space (virtiofs statfs is the host's)
	local free
	while free=$(df -BG --output=avail "$OUT" | tail -1 | tr -dc 0-9); [ "$free" -lt "$MAC_MIN_GB" ]; do
		echo "[$1] the Mac has ${free} GB free (< $MAC_MIN_GB): waiting"; sleep 60
	done
}
mac_wait start
vm_used0=$(df -BM --output=used / | tail -1 | tr -dc 0-9)
vm_guard() {                                        # vm_guard STEP
	local now; now=$(df -BM --output=used / | tail -1 | tr -dc 0-9)
	echo "[$1] VM disk +$(( now - vm_used0 )) MiB, image: $(df -BM --output=used,avail "$R" 2>/dev/null | tail -1)"
	[ $(( now - vm_used0 )) -le "$VM_MAX_MB" ] || { echo "VM disk grew by more than $VM_MAX_MB MiB: stopping" >&2; exit 1; }
}
again() { "$@" || { sleep 3; "$@"; }; }             # virtiofs: a fresh file can be missing for a moment

R=/mnt/sb-$NAME
T=/mnt/sb-$NAME-tmp
LOOP=
CLOOP=
cleanup() {
	set +e
	for m in "$R/tmp/sb" "$R/var/cache/apt/archives" "$R/dev/pts" "$R/dev" "$R/proc" "$R/sys" "$R/run" "$R"; do mountpoint -q "$m" && umount "$m"; done
	[ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null
	[ -n "$CLOOP" ] && losetup -d "$CLOOP" 2>/dev/null
	mountpoint -q "$T" && umount "$T"
	rmdir "$T" 2>/dev/null
	rm -rf "$TMPB"
}
trap cleanup EXIT
mkdir -p "$R" "$T" "$TMPB"
mount -t tmpfs -o size=512m,mode=755 tmpfs "$T"                 # 755: apt's gpgv runs as _apt and reads the keyring

# ---- the Kali archive key: HTTPS, then its fingerprint ------------------------------------------
KEYRING=$T/kali-archive-keyring.gpg
curl -fsSL --proto '=https' "$KEY_URL" -o "$KEYRING"; chmod 644 "$KEYRING"
fprs=$(GNUPGHOME=$(mktemp -d "$T/gpg.XXXX") gpg --batch --show-keys --with-colons "$KEYRING" 2>/dev/null |
	awk -F: '$1 == "fpr" {print $10}')
echo "$fprs" | grep -qx "$KALI_FPR" || { echo "Kali keyring from $KEY_URL lacks $KALI_FPR (has: $fprs)" >&2; exit 1; }
cp "$KEYRING" "$TMPB/" 2>/dev/null || true
echo "Kali archive key $KALI_FPR ok ($(sha256sum "$KEYRING" | cut -c1-16)...)"

# ---- the image: a sparse file on the Mac --------------------------------------------------------
rm -f "$IMG" "$IMG.sha256"
truncate -s "$SIZE" "$IMG"
again mkfs.ext4 -q -F -E nodiscard -O ^orphan_file,^metadata_csum_seed -L sb-$NAME -m 0 "$IMG"
LOOP=$(again losetup -f --show "$IMG")
mount -o noatime "$LOOP" "$R"
vm_guard image

# ---- bootstrap ----------------------------------------------------------------------------------
nodoc=()
if [ "$NODOC" = 1 ]; then
	nodoc=(--dpkgopt='path-exclude=/usr/share/doc/*' --dpkgopt='path-include=/usr/share/doc/*/copyright'
		--dpkgopt='path-exclude=/usr/share/man/*' --dpkgopt='path-exclude=/usr/share/info/*'
		--dpkgopt='path-exclude=/usr/share/help/*' --dpkgopt='path-include=/usr/share/help/C/*'
		--dpkgopt='path-exclude=/usr/share/locale/*' --dpkgopt='path-include=/usr/share/locale/en*'
		--dpkgopt='path-include=/usr/share/locale/locale.alias' --dpkgopt='path-exclude=/usr/share/lintian/*')
fi
mac_wait mmdebstrap
TMPDIR=$T mmdebstrap --mode=root --arch=arm64 --variant=minbase --keyring="$KEYRING" \
	--aptopt='APT::Install-Recommends "false"' --dpkgopt=force-unsafe-io "${nodoc[@]}" \
	--include=kali-archive-keyring,ca-certificates,systemd-sysv,dbus,udev,locales,apt-utils,debconf-utils \
	--components="main contrib non-free non-free-firmware" \
	"$SUITE" "$R" "$MIRROR"
fstrim "$R" >/dev/null 2>&1 || true
vm_guard mmdebstrap
# unsafe-io was for the build only: the phone's dpkg syncs (a hard reset mid-upgrade)
sed -i '/^force-unsafe-io$/d' "$R"/etc/dpkg/dpkg.cfg.d/99mmdebstrap 2>/dev/null || true

mount --bind /dev "$R/dev"; mount --bind /dev/pts "$R/dev/pts"
mount -t proc proc "$R/proc"; mount -t sysfs sys "$R/sys"; mount -t tmpfs tmpfs "$R/run"
rm -f "$R/etc/resolv.conf"; cp /etc/resolv.conf "$R/etc/resolv.conf"
printf '#!/bin/sh\nexit 101\n' > "$R/usr/sbin/policy-rc.d"; chmod 755 "$R/usr/sbin/policy-rc.d"
# apt's downloads go to a second sparse ext4 on the Mac (TMPB, deleted at the end), not into the
# image: metasploit's 470 MB of .debs would not fit next to what they unpack to
CIMG=$TMPB/apt-archives.img
rm -f "$CIMG"; truncate -s 2G "$CIMG"
again mkfs.ext4 -q -F -E nodiscard -m 0 "$CIMG"
CLOOP=$(again losetup -f --show "$CIMG")
mount -o noatime "$CLOOP" "$R/var/cache/apt/archives"
mkdir -p "$R/var/cache/apt/archives/partial"

# ---- apt: kali-rolling signed by the keyring package; Starbite's no-kernel pin first ------------
[ -s "$R/usr/share/keyrings/kali-archive-keyring.gpg" ] || { echo "kali-archive-keyring not installed" >&2; exit 1; }
rm -f "$R/etc/apt/sources.list"
cat > "$R/etc/apt/sources.list.d/kali.sources" <<EOF
Types: deb
URIs: http://http.kali.org/kali
Suites: $SUITE
Components: main contrib non-free non-free-firmware
Signed-By: /usr/share/keyrings/kali-archive-keyring.gpg
EOF
mkdir -p "$R/etc/apt/preferences.d" "$R/etc/apt/apt.conf.d"
sed -n '/^Package: linux-image/,/^Pin-Priority/p' "$HERE/sb-integrate.sh" > "$R/etc/apt/preferences.d/90-starbite-no-kernel"
grep -q 'Pin-Priority: -1' "$R/etc/apt/preferences.d/90-starbite-no-kernel"
printf 'APT::Install-Recommends "false";\n' > "$R/etc/apt/apt.conf.d/90-starbite-no-recommends"
in_root() { chroot "$R" /usr/bin/env DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8 TMPDIR=/tmp "$@"; }
# non-root capture for the wheel/wireshark group
echo 'wireshark-common wireshark-common/install-setuid boolean true' | in_root debconf-set-selections
in_root apt-get -q update

# install STAGE PKGS...: only what this suite has; refuse when it does not fit in the image
install_stage() {
	local stage=$1; shift
	local avail=() p new need get free cfree
	mac_wait "$stage"
	for p in "$@"; do
		if in_root apt-cache show "$p" >/dev/null 2>&1; then avail+=("$p"); else echo "[$stage] not in $SUITE: $p (skipped)"; fi
	done
	# simulate only (apt-get --assume-no still installs when there is nothing to ask about)
	new=$(in_root apt-get -s -q install "${avail[@]}" | awk '$1 == "Inst" {print $2}')
	# shellcheck disable=SC2086
	need=$( [ -z "$new" ] && echo 0 || in_root apt-cache show --no-all-versions $new 2>/dev/null |
		awk '/^Installed-Size:/ {k += $2} END {print int(k / 1024)}')
	get=$(in_root apt-get -qq --print-uris install "${avail[@]}" | awk '{b += $3} END {print int(b / 1048576)}')
	free=$(df -BM --output=avail "$R" | tail -1 | tr -dc 0-9)
	cfree=$(df -BM --output=avail "$R/var/cache/apt/archives" | tail -1 | tr -dc 0-9)
	echo "[$stage] $(echo $new | wc -w) packages, ${need} MiB installed (${free} MiB free in the image), ${get} MiB downloads (${cfree} MiB free for them)"
	[ $(( need + 100 )) -le "$free" ] || { echo "[$stage] does not fit in $SIZE" >&2; exit 1; }
	[ $(( get + 50 )) -le "$cfree" ] || { echo "[$stage] downloads do not fit in the apt cache image" >&2; exit 1; }
	in_root apt-get -q -y -o Dpkg::Options::=--force-unsafe-io install "${avail[@]}"
	in_root apt-get -q -y clean
	fstrim "$R/var/cache/apt/archives" >/dev/null 2>&1 || true   # deleted downloads: holes again in the Mac's sparse file
	vm_guard "$stage"
}
install_stage core $CORE
install_stage ui $UI
install_stage tools $TOOLS
if [ "$MSF" = 1 ]; then
	msfdeps=$(in_root apt-get -s -q install metasploit-framework | awk '$1 == "Inst" && $2 != "metasploit-framework" {print $2}')
	# shellcheck disable=SC2086
	[ -z "$msfdeps" ] || { install_stage msf-deps $msfdeps; in_root apt-mark auto $msfdeps >/dev/null; }
	install_stage msf metasploit-framework
fi
rm -rf "$R"/var/lib/apt/lists/* "$R"/var/cache/apt/*.bin

# ---- Starbite integration (shared with every distro); the payload (PIN) only ever in RAM ----------
mkdir -p "$R/tmp/sb"
mount -t tmpfs -o mode=700 tmpfs "$R/tmp/sb"
cp "$HERE/sb-integrate.sh" "$R/tmp/sb/"
UH=$UH sh "$HERE/mk-payload.sh" "$R/tmp/sb/payload" $NAME
chroot "$R" /bin/sh /tmp/sb/sb-integrate.sh $NAME systemd /tmp/sb/payload
umount "$R/tmp/sb"; rmdir "$R/tmp/sb"
echo phosh > "$R/etc/starbite/session"               # sb.session=phosh is the only session here
# the shared modules image's mount point (the initrd binds /distros/_kernel/modules-REL.img there):
# with sb.rw=0 the root is read-only and the initrd cannot mkdir it, so it is made here, empty.
# Where the initrd's sb_mods_dir puts it: usr/lib/modules when /lib is a link, else lib/modules.
KREL=${KREL:-$(cat "$P/build/linux/release")}
[ -n "$KREL" ] || { echo "no kernel release in $P/build/linux/release" >&2; exit 1; }
if [ -L "$R/lib" ]; then MODDIR=usr/lib/modules/$KREL; else MODDIR=lib/modules/$KREL; fi
mkdir -p "$R/$MODDIR"
echo "sessions installed: $(in_root dpkg-query -W -f='${Package}\n' phosh 2>/dev/null | tr '\n' ' ')" >> "$R/etc/starbite/integrated"

# ---- Kali specifics -----------------------------------------------------------------------------
# packet capture as "user" (wireshark-common's setuid dumpcap is for group wireshark)
chroot "$R" sh -c 'getent group wireshark >/dev/null && usermod -aG wireshark user' || true
# DNS: NetworkManager writes /etc/resolv.conf (no systemd-resolved here)
rm -f "$R/etc/resolv.conf"; : > "$R/etc/resolv.conf"
# Kali's network-service policy leaves daemons it installs disabled; postgresql (metasploit's db)
# stays that way: "sudo msfdb init" when it is wanted. sb-integrate enabled sshd explicitly.
rm -f "$R/usr/sbin/policy-rc.d"
# nothing of the build host stays
rm -f "$R/etc/ssh/ssh_host_"*                        # sb-ssh-keys.service makes the phone's own
rm -rf "$R"/tmp/* "$R"/var/tmp/* "$R"/var/log/*.log "$R"/var/log/apt/* "$R"/var/cache/apt/archives/*.deb
: > "$R/etc/machine-id"

# ---- checks inside the root ---------------------------------------------------------------------
fail=0
chk() { if chroot "$R" /bin/sh -c "$2" >/dev/null 2>&1; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }
chk "os-release is Kali"          "grep -qx 'ID=kali' /etc/os-release"
chk "no kernel/bootloader pkgs"   "! dpkg-query -W -f='\${Status} \${Package}\n' 'linux-image*' 'linux-modules*' 'grub*' 'flash-kernel' 'u-boot*' 2>/dev/null | grep -q '^install ok installed'"
chk "no /boot kernel"             "! ls /boot/vmlinu* /boot/initrd* 2>/dev/null | grep -q ."
chk "kernel pin -1"               "grep -q '^Package: linux-image' /etc/apt/preferences.d/90-starbite-no-kernel && grep -qx 'Pin-Priority: -1' /etc/apt/preferences.d/90-starbite-no-kernel"
chk "user 10000"                  "[ \"\$(id -u user):\$(id -g user)\" = 10000:10000 ]"
chk "user groups"                 "id -nG user | tr ' ' '\n' | grep -qx render && id -nG user | tr ' ' '\n' | grep -qx sudo"
chk "phosh-session"               "[ -x /usr/bin/phosh-session ] && [ -x /usr/bin/phoc ]"
chk "starbite-session enabled"    "systemctl is-enabled starbite-session.service | grep -qx enabled"
chk "ssh.service enabled"         "systemctl is-enabled ssh.service | grep -qx enabled"
chk "ssh.socket masked"           "systemctl is-enabled ssh.socket | grep -qx masked"
chk "sb-ssh-keys enabled"         "systemctl is-enabled sb-ssh-keys.service | grep -qx enabled && [ -x /usr/local/sbin/sb-ssh-keys ]"
chk "no host keys baked in"       "! ls /etc/ssh/ssh_host_* 2>/dev/null | grep -q ."
chk "networkd + NM enabled"       "systemctl is-enabled systemd-networkd.service | grep -qx enabled && systemctl is-enabled NetworkManager.service | grep -qx enabled"
chk "ModemManager masked"         "systemctl is-enabled ModemManager.service 2>&1 | grep -qx masked"
chk "default graphical"           "systemctl get-default | grep -qx graphical.target"
chk "Wi-Fi firmware"              "[ -s /usr/lib/firmware/brcm/brcmfmac4361-pcie.bin ]"
chk "tools present"               "for b in nmap hydra john nxc responder sqlmap tshark \$([ $MSF = 1 ] && echo msfconsole); do command -v \$b || exit 1; done"
chk "modules mount point"         "[ -d '/$MODDIR' ] && [ -z \"\$(ls -A '/$MODDIR')\" ] && ! ls -d /usr/lib/modules/*/kernel /lib/modules/*/kernel 2>/dev/null | grep -q ."
chk "mesa panfrost"               "ls /usr/lib/aarch64-linux-gnu/dri/panfrost_dri.so || ls /usr/lib/aarch64-linux-gnu/libgallium-*.so"
(cd "$R" && grep -h '^PRETTY_NAME\|^VERSION=' etc/os-release; cat etc/starbite/integrated)
[ "$fail" = 0 ] || { echo "checks failed (image left at $IMG)" >&2; exit 1; }

# ---- unmount, check, shrink ---------------------------------------------------------------------
for m in "$R/var/cache/apt/archives" "$R/dev/pts" "$R/dev" "$R/proc" "$R/sys" "$R/run"; do umount "$m"; done
losetup -d "$CLOOP"; CLOOP=
used=$(df -BM --output=used "$R" | tail -1 | tr -dc 0-9)
umount "$R"
e2fsck -fy "$LOOP" >/dev/null || [ $? -le 1 ]
resize2fs -M "$LOOP" >/dev/null 2>&1
e2fsck -fy "$LOOP" >/dev/null || [ $? -le 1 ]
bs=$(dumpe2fs -h "$LOOP" 2>/dev/null | awk -F: '/^Block size/ {print $2+0}')
blocks=$(dumpe2fs -h "$LOOP" 2>/dev/null | awk -F: '/^Block count/ {print $2+0}')
losetup -d "$LOOP"; LOOP=
truncate -s $(( blocks * bs )) "$IMG"
sync
(cd "$(dirname "$IMG")" && sha256sum "$(basename "$IMG")") > "$IMG.sha256"
vm_guard done
echo "$IMG: $(( blocks * bs / 1048576 )) MiB ext4 (sb-$NAME), ${used} MiB of files; grow it on the card (card-install.sh)"
echo "manifest: $NAME $(( blocks * bs / 1048576 ))M kali-rolling $(cut -d' ' -f1 "$IMG.sha256") $(date -I) 7.1.0-starbite"
