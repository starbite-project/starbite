#!/usr/bin/env bash
# Builds Starbite Maintenance (docs/MAINTENANCE.md 8) in the arm64 Lima VM, like tools/linux/mk-initrd.sh:
#
#   limactl shell starbite -- sudo bash /Users/dev/project-starbite/tools/maint/env/mk-maint.sh [OUTDIR]
#
# OUTDIR (default ~/distros/out/maint in the VM) gets:
#   maint.initrd      Alpine minirootfs + partition/filesystem tools (sgdisk gdisk blkid e2fsprogs f2fs-tools
#                     exfatprogs dosfstools ntfs-3g(+progs) erofs-utils squashfs-tools losetup partx sfdisk
#                     wipefs) + zstd/gzip/pigz/xz/lz4 + coreutils + dropbear + usbutils + kbd's setfont,
#                     Alpine's busybox as the shell, Ubuntu's static busybox (as busybox.static) for the
#                     applets Alpine's lacks (devmem udhcpd ...), memdump (built here, static), and
#                     tools/maint/env/* + tools/rescue/init (as sb-rescue) + tools/linux/reboot-to.
#                     newc cpio | zstd -19 (COMP=gzip for a kernel without RD_ZSTD).
#   maint.dtb         the daily DTB: USB device (dr_mode peripheral), Linux's ramoops kept off the log
#                     rings (compatible renamed: the region stays reserved, the previous boot's rings and
#                     the boot-next mailbox survive for `sbm logs`), "console=ramoops-1" dropped
#   maint-rescue.dtb  the store's rescue.dtb (USB host by default, OTG VBUS) with the same ramoops change
#   maint.image       a copy of the kernel (KDIR/Image)
#   sbmaint.img       the SBMAINT1 container of all four (tools/maint/pack.py), seq $SEQ
#   store/            maint.image maint.dtb maint.initrd maint-rescue.dtb: the OS-store entries
#   known_hosts       "sbmaint,172.16.42.1 ssh-ed25519 ..." for the computer (sbm pins the host key)
#   SHA256SUMS, BUILD-INFO, packages.txt
# and copies them (not maint.image/store) to COPY_TO (default <main checkout>/build/maint).
#
# Inputs (environment): KDIR (default ~/distros/_kernel-mask: 7.1.0-starbite-4da38c67, the store's
# linux.image since 2026-11-06) with Image, exynos9810-starlte.dtb, rescue.dtb, kernel.config;
# BUSYBOX (/mnt/build/out/busybox-arm64); KEYS (ssh public keys baked in; default the Mac's
# build/linux/ssh/starbite_ed25519.pub and the installer's build/installer/env/id_ed25519.pub);
# HOSTKEY_DIR (default <main>/build/maint/keys: the environment's ssh host key, made once and kept, so
# the computer can pin it); SEQ (1); CMDLINE (container cmdline); RAMOOPS (keep | linux); COMP.
# NOT RUN ON THE PHONE YET (2026-11-06).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
MAIN=${MAIN:-${REPO%/.claude/worktrees/*}}
U=${SUDO_USER:-$(id -un)}
UH=$(getent passwd "$U" | cut -d: -f6)
AV=${ALPINE_VERSION:-3.24.3}
AB=v${AV%.*}
MIRROR=${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}
CACHE=${CACHE:-$UH/distros/cache}
OUT=${1:-$UH/distros/out/maint}
KDIR=${KDIR:-$UH/distros/_kernel-mask}
BUSYBOX=${BUSYBOX:-/mnt/build/out/busybox-arm64}
KEYS=${KEYS:-$MAIN/build/linux/ssh/starbite_ed25519.pub $MAIN/build/installer/env/id_ed25519.pub}
HOSTKEY_DIR=${HOSTKEY_DIR:-$MAIN/build/maint/keys}
COPY_TO=${COPY_TO:-$MAIN/build/maint}
SEQ=${SEQ:-1}
CMDLINE=${CMDLINE:-s9p_decon.panel_mask=2 s9p_decon.geom_log=0}
RAMOOPS=${RAMOOPS:-keep}
COMP=${COMP:-zstd}
FONT=${FONT:-/usr/share/consolefonts/Uni2-TerminusBold32x16.psf.gz}
PKGS="e2fsprogs e2fsprogs-extra exfatprogs f2fs-tools dosfstools sgdisk gptfdisk blkid losetup util-linux-misc
      sfdisk partx lsblk wipefs findmnt ntfs-3g ntfs-3g-progs erofs-utils squashfs-tools zstd lz4 xz gzip pigz
      coreutils dropbear usbutils kbd"
TOOLS="sh sgdisk gdisk blkid mke2fs mkfs.ext4 e2fsck resize2fs tune2fs dumpe2fs debugfs mkfs.f2fs fsck.f2fs
       mkfs.exfat fsck.exfat mkfs.vfat fsck.vfat ntfs-3g ntfsresize ntfsfix ntfsinfo mkfs.erofs fsck.erofs
       mksquashfs unsquashfs losetup blockdev partx sfdisk wipefs lsblk findmnt zstd gzip pigz xz lz4 sha256sum
       dd head readlink stat mknod dropbear dropbearkey udhcpd devmem setsid loadfont setfont hexdump netstat
       lsusb tar cpio mount umount memdump sb-byname sb-protect sb-reboot sb-usb sb-status sb-info sb-job
       getprop sb-rescue reboot-to"

[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 1; }
[ "$(uname -m)" = aarch64 ] || { echo "needs an arm64 host (the Lima VM)" >&2; exit 1; }
file "$BUSYBOX" | grep -q 'statically linked' || { echo "$BUSYBOX is not a static busybox" >&2; exit 1; }
for a in devmem udhcpd setsid loadfont hexdump netstat; do
	"$BUSYBOX" --list | grep -qx $a || { echo "$BUSYBOX has no $a" >&2; exit 1; }
done
for f in Image exynos9810-starlte.dtb rescue.dtb kernel.config release; do
	[ -s "$KDIR/$f" ] || { echo "missing $KDIR/$f (tools/linux/build-kernel.sh)" >&2; exit 1; }
done
KCONFIG=$KDIR/kernel.config
for c in USB_CONFIGFS_ECM USB_CONFIGFS_NCM USB_CONFIGFS_ACM USB_DWC3 EXFAT_FS EXT4_FS VFAT_FS FUSE_FS BLK_DEV_INITRD \
         DEVTMPFS SCSI_UFS_EXYNOS MMC_DW_EXYNOS MAGIC_SYSRQ DEVMEM USB_STORAGE BLK_DEV_LOOP EFI_PARTITION FB FRAMEBUFFER_CONSOLE; do
	grep -q "^CONFIG_$c=y" "$KCONFIG" || { echo "the kernel lacks CONFIG_$c=y ($KCONFIG)" >&2; exit 1; }
done
[ "$COMP" != zstd ] || grep -q '^CONFIG_RD_ZSTD=y' "$KCONFIG" || { echo "no CONFIG_RD_ZSTD: COMP=gzip" >&2; exit 1; }
grep -q '^CONFIG_F2FS_FS=y' "$KCONFIG" || echo "note: this kernel cannot MOUNT f2fs (mkfs.f2fs / fsck.f2fs work on the raw partition)"
grep -q '^CONFIG_NTFS3_FS=y' "$KCONFIG" || echo "note: no in-kernel NTFS; ntfs-3g (FUSE) mounts NTFS"
set -- $KEYS
nk=0
for k; do [ -s "$k" ] && nk=$((nk + 1)); done
[ $nk -gt 0 ] || { echo "no ssh public key found in: $KEYS" >&2; exit 1; }

mkdir -p "$CACHE" "$OUT" "$OUT/store"
TGZ=alpine-minirootfs-$AV-aarch64.tar.gz
[ -s "$CACHE/$TGZ" ] || { curl -fsSL -o "$CACHE/$TGZ.part" "$MIRROR/$AB/releases/aarch64/$TGZ"; mv "$CACHE/$TGZ.part" "$CACHE/$TGZ"; }
SHA=$(curl -fsSL "$MIRROR/$AB/releases/aarch64/$TGZ.sha256" | awk '{print $1}')
echo "$SHA  $CACHE/$TGZ" | sha256sum -c --quiet || { rm -f "$CACHE/$TGZ"; echo "bad $TGZ" >&2; exit 1; }

W=$(mktemp -d /var/tmp/sb-maint.XXXX)
R=$W/r
cleanup() { for m in "$R/proc" "$R/dev"; do mountpoint -q "$m" && umount "$m"; done; rm -rf "$W"; }
trap cleanup EXIT
mkdir -p "$R"
tar -xzpf "$CACHE/$TGZ" -C "$R"

# ---- packages (apk checks their signatures against the keys in the minirootfs) -------------------
printf '%s/%s/main\n%s/%s/community\n' "$MIRROR" "$AB" "$MIRROR" "$AB" > "$R/etc/apk/repositories"
cp /etc/resolv.conf "$R/etc/resolv.conf"
mount -t proc proc "$R/proc"
mount --bind /dev "$R/dev"
chroot "$R" /sbin/apk add --no-cache --no-progress $PKGS >/dev/null
chroot "$R" /sbin/apk info -v 2>/dev/null | sort > "$W/packages.txt"

# the environment's ssh host key: made once (here, by dropbear) and kept, so the computer can pin it
mkdir -p "$HOSTKEY_DIR"
HK=$HOSTKEY_DIR/dropbear_ed25519_host_key
if [ ! -s "$HK" ]; then
	chroot "$R" dropbearkey -t ed25519 -f /tmp/hk >/dev/null
	install -m600 "$R/tmp/hk" "$HK"
	chown "$U": "$HK" "$HOSTKEY_DIR" 2>/dev/null || true       # virtiofs may refuse: harmless
	echo "made a new ssh host key: $HK"
fi
install -D -m600 "$HK" "$R/etc/dropbear/dropbear_ed25519_host_key"
HOSTPUB=$(chroot "$R" dropbearkey -y -f /etc/dropbear/dropbear_ed25519_host_key | grep '^ssh-ed25519' | awk '{print $1 " " $2}')

# ext4 for Android's 4.9 kernel: no orphan_file (the reason metadata-reset.sh used TWRP's mke2fs)
install -m644 "$HERE/mke2fs.conf" "$R/etc/mke2fs.conf"
chroot "$R" sh -c 'truncate -s 64M /tmp/t.img && mke2fs -F -q -t ext4 -b 4096 -m 0 -L metadata /tmp/t.img &&
	dumpe2fs -h /tmp/t.img 2>/dev/null | sed -n "s/^Filesystem features: *//p"; rm -f /tmp/t.img' > "$W/ext4-features"
grep -qw has_journal "$W/ext4-features" || { echo "mke2fs test failed: $(cat "$W/ext4-features")" >&2; exit 1; }
for x in orphan_file metadata_csum_seed fast_commit stable_inodes verity; do
	! grep -qw $x "$W/ext4-features" || { echo "mke2fs.conf still gives $x (Android's 4.9 kernel cannot mount it)" >&2; exit 1; }
done
umount "$R/proc" "$R/dev"

# ---- trim (no package manager, docs, keymaps or fonts in the ramdisk) ----------------------------
rm -rf "$R"/etc/apk "$R"/lib/apk "$R"/var/cache/apk "$R"/usr/share/apk "$R"/sbin/apk \
	"$R"/usr/share/man "$R"/usr/share/doc "$R"/usr/share/info "$R"/usr/include "$R"/usr/share/misc \
	"$R"/usr/share/kbd "$R"/usr/share/keymaps "$R"/usr/share/consolefonts "$R"/usr/share/consoletrans \
	"$R"/usr/share/unimaps "$R"/etc/resolv.conf "$R"/media "$R"/srv "$R"/opt "$R"/home "$R"/tmp/*
find "$R" \( -name '*.a' -o -name '*.la' \) -delete

# ---- busybox: Alpine's stays /bin/sh; Ubuntu's static one only for the applets Alpine's lacks -------
# (devmem, udhcpd ...). NOT as the shell: Ubuntu's busybox-static ash runs its own applets in preference
# to PATH (FEATURE_SH_STANDALONE), so `mke2fs`, `blkid`, `blockdev` in a script would silently be busybox's,
# not e2fsprogs'/util-linux's. Checked below.
rm -f "$R"/lib/libapk.so*
install -m755 "$BUSYBOX" "$R/bin/busybox.static"
for a in $("$BUSYBOX" --list); do
	case $a in busybox|linuxrc|init|sh|ash) continue ;; esac
	found=
	for d in bin sbin usr/bin usr/sbin; do
		{ [ -e "$R/$d/$a" ] || [ -L "$R/$d/$a" ]; } && { found=1; break; }
	done
	[ -n "$found" ] || ln -s busybox.static "$R/bin/$a"
done

# ---- memdump (static; tools/maint/env/memdump.c) ----------------------------------------------------
gcc -static -O2 -Wall -Werror -o "$W/memdump" "$HERE/memdump.c"
install -D -m755 "$W/memdump" "$R/usr/local/sbin/memdump"

# ---- Starbite files ---------------------------------------------------------------------------------
install -m755 "$HERE/init" "$R/init"
for t in sb-byname sb-protect sb-reboot sb-usb sb-status sb-info sb-job getprop; do install -m755 "$HERE/$t" "$R/bin/$t"; done
install -m755 "$REPO/tools/rescue/init" "$R/bin/sb-rescue"
install -m755 "$REPO/tools/linux/reboot-to" "$R/bin/reboot-to"
mkdir -p "$R/usr/share/sbmaint"
if [ -s "$FONT" ]; then
	python3 "$HERE/mkfont.py" "$FONT" "$R/usr/share/sbmaint/font-16x32.psf" --scale 1
	python3 "$HERE/mkfont.py" "$FONT" "$R/usr/share/sbmaint/font-32x64.psf" --scale 2
else
	echo "note: no $FONT (console-setup): the screen keeps fbcon's 8x16"
fi
mkdir -p "$R/etc/sbmaint" "$R/root/.ssh" "$R/external_sd" "$R"/proc "$R"/sys "$R"/dev "$R"/run "$R"/tmp
for k in $KEYS; do [ -s "$k" ] && { cat "$k"; echo; }; done | grep -E '^(ssh-ed25519|ssh-rsa|ecdsa-sha2-nistp256) ' \
	| sort -u > "$R/etc/sbmaint/authorized_keys"
chmod 700 "$R/root" "$R/root/.ssh"
sed -i 's/^root:[^:]*:/root:*:/' "$R/etc/shadow"           # not empty, not "!": dropbear's key login works
grep -qx /bin/sh "$R/etc/shells" 2>/dev/null || echo /bin/sh >> "$R/etc/shells"
echo sbmaint > "$R/etc/hostname"
GIT=$(git -c safe.directory='*' -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)
REL="sbmaint $(date -u +%Y-%m-%d) $GIT"
echo "$REL" > "$R/etc/sbmaint-release"
echo "Starbite Maintenance ($REL), kernel $(cat "$KDIR/release")" > "$R/etc/starbite-env"
cat > "$R/etc/profile.d/sbmaint.sh" <<'EOF'
export PS1='sbmaint:\w# '
if [ -t 1 ]; then
	echo "$(head -1 /etc/sbmaint-release): Starbite Maintenance (docs/MAINTENANCE.md)"
	echo "sb-info  sb-job TEXT  sb-protect list|allow NAME  sb-byname  sb-reboot menu|maint|fastboot|KEY"
fi
EOF
[ -e "$R/dev/console" ] || mknod -m 600 "$R/dev/console" c 5 1
[ -e "$R/dev/null" ] || mknod -m 666 "$R/dev/null" c 1 3
missing=
for t in $TOOLS; do chroot "$R" /bin/sh -c "command -v $t" >/dev/null 2>&1 || missing="$missing $t"; done
[ -z "$missing" ] || { echo "missing in the ramdisk:$missing" >&2; exit 1; }
# the shell runs what PATH says (not its own applets), and the real tools come first on an ssh session's
# PATH (dropbear's for root: /usr/sbin:/usr/bin:/sbin:/bin)
printf '#!/bin/sh\necho from-path\n' > "$R/tmp/blockdev"
chmod 755 "$R/tmp/blockdev"
[ "$(chroot "$R" /bin/sh -c 'PATH=/tmp:/usr/sbin:/usr/bin:/sbin:/bin; blockdev')" = from-path ] ||
	{ echo "/bin/sh prefers its own applets over PATH" >&2; exit 1; }
rm -f "$R/tmp/blockdev"
for t in mke2fs e2fsck tune2fs blkid mkfs.vfat fsck.vfat mkfs.exfat losetup sgdisk dd head sha256sum stat lsblk; do
	p=$(chroot "$R" /bin/sh -c "PATH=/usr/sbin:/usr/bin:/sbin:/bin; command -v $t")
	case $(readlink -f "$R$p") in */busybox|*/busybox.static) echo "$t resolves to busybox ($p), not the real tool" >&2; exit 1 ;; esac
done
{
	echo "$REL"
	echo "kernel $(cat "$KDIR/release") from $KDIR"
	echo "alpine-minirootfs $AV ($SHA)"
	echo "busybox: Alpine's (the shell); $("$BUSYBOX" | head -1) as busybox.static for devmem, udhcpd ..."
	echo "ext4 defaults: $(cat "$W/ext4-features")"
	echo "ssh keys: $(wc -l < "$R/etc/sbmaint/authorized_keys"); host key $HOSTPUB"
	echo "container cmdline: $CMDLINE"
} > "$OUT/BUILD-INFO"
cp "$W/packages.txt" "$OUT/packages.txt"

case $COMP in zstd) Z="zstd -19 -T0 -q" ;; gzip) Z="gzip -9" ;; *) echo "COMP zstd|gzip" >&2; exit 1 ;; esac
(cd "$R" && find . -mindepth 1 | LC_ALL=C sort | cpio -o -H newc -R 0:0 --quiet) | $Z > "$OUT/maint.initrd"
echo "maint.initrd: $(( $(stat -c %s "$OUT/maint.initrd") / 1024 )) KiB ($COMP), unpacked $(du -sh "$R" | cut -f1)"

# ---- DTBs -------------------------------------------------------------------------------------------
dtb_edit() {                                       # dtb_edit IN OUT peripheral|otg
	dtc -q -I dtb -O dts -o "$W/e.dts" "$1"
	python3 - "$W/e.dts" "$3" "$RAMOOPS" <<'PY'
import re, sys
p, mode, ramoops = sys.argv[1:]
s = open(p).read()
def one(old, new):
    global s
    n = s.count(old)
    if n != 1:
        sys.exit(f'dtb_edit: expected 1x {old!r}, found {n}')
    s = s.replace(old, new)
if ramoops == 'keep':
    one('compatible = "ramoops";', 'compatible = "starbite,ramoops-kept";')
    if 'console=ramoops-1 ' in s:
        one('console=ramoops-1 ', '')
if mode == 'peripheral':
    s = s.replace('dr_mode = "host";', 'dr_mode = "peripheral";').replace('dr_mode = "otg";', 'dr_mode = "peripheral";')
    if s.count('dr_mode = "peripheral";') != 1:
        sys.exit('dtb_edit: the USB controller is not a single peripheral')
elif not re.search(r'dr_mode = "(otg|host)";', s):
    sys.exit('dtb_edit: the rescue DTB is not USB host/otg')
for prop in ('linux,initrd-start', 'linux,initrd-end', 'bootargs', 'starbite,rtc-epoch'):
    if prop not in s:
        sys.exit(f'dtb_edit: no /chosen {prop}')
open(p, 'w').write(s)
PY
	dtc -q -I dts -O dtb -o "$2" "$W/e.dts"
}
dtb_edit "$KDIR/exynos9810-starlte.dtb" "$OUT/maint.dtb" peripheral
dtb_edit "$KDIR/rescue.dtb" "$OUT/maint-rescue.dtb" otg
cp "$KDIR/Image" "$OUT/maint.image"

# ---- the container, the store entries, the computer's known_hosts ----------------------------------
python3 "$REPO/tools/maint/pack.py" pack "$OUT/sbmaint.img" --kernel "$OUT/maint.image" --dtb "$OUT/maint.dtb" \
	--initrd "$OUT/maint.initrd" --dtb-rescue "$OUT/maint-rescue.dtb" --cmdline "$CMDLINE" --version "$REL" --seq "$SEQ"
python3 "$REPO/tools/maint/pack.py" verify "$OUT/sbmaint.img" > /dev/null
python3 "$REPO/tools/maint/pack.py" store-entries "$OUT/sbmaint.img" "$OUT/store" > /dev/null
echo "sbmaint,172.16.42.1 $HOSTPUB" > "$OUT/known_hosts"
(cd "$OUT" && sha256sum sbmaint.img maint.image maint.initrd maint.dtb maint-rescue.dtb known_hosts) > "$OUT/SHA256SUMS"
chown -R "$U": "$OUT"
if [ -n "$COPY_TO" ]; then
	mkdir -p "$COPY_TO"
	for f in sbmaint.img maint.initrd maint.dtb maint-rescue.dtb known_hosts SHA256SUMS BUILD-INFO packages.txt; do
		cp "$OUT/$f" "$COPY_TO/$f"
	done
	chown -R "$U": "$COPY_TO" 2>/dev/null || true
	echo "copied to $COPY_TO"
fi
cat "$OUT/SHA256SUMS"
