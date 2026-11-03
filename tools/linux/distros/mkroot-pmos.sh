#!/usr/bin/env bash
# postmarketOS (edge / pmaports master, OpenRC) root for the S9: /distros/pmos.img on the card's
# exFAT (p1), ext4, label sb-pmos (docs/LINUX-REBUILD-PLAN.md 3.5 distro 2). Built in the arm64
# Lima VM on the Mac's APFS disk through the VM's rw mount of the main checkout (git-ignored
# build/distros-build): sparse, and the space comes back when the file is deleted. Never on the
# VM's own disk (a never-shrinking file on the Mac) or the exFAT stick (no sparse files, every
# image zero-filled first). The VM sees /Users/dev read-only, so not ~/distros-build:
#
#   limactl shell starbite -- sudo bash /Users/dev/project-starbite/tools/linux/distros/mkroot-pmos.sh [UI]
#
# UI: phosh (default) | plasma-mobile | sxmo-de-sway (postmarketos-ui-UI). NAME (default pmos).
# What it does, in order:
#   apk.static from Alpine edge over HTTPS: the same sha256 from two mirrors (and APK_SHA when
#     given), then its own detached RSA-SHA256 signature checked with openssl against the Alpine
#     key from pmbootstrap (build/linux/keys, as pmbootstrap itself does)
#   repositories: postmarketOS master + Alpine edge main/community/testing, keys from build/linux/keys
#   postmarketos-base (OpenRC flavour) + postmarketos-ui-UI, no splash, wpa_supplicant, doas
#   NO device package, kernel, initramfs generator or boot-deploy: a local unsigned package
#     "starbite-pmos-shim" (apk mkpkg, installed once with --allow-untrusted, alone) provides
#     postmarketos-ramdisk (else postmarketos-initramfs -> postmarketos-mkinitfs -> boot-deploy)
#     and postmarketos-usb-moded (usb-moded / usb-signaller reconfigure the USB gadget: usb0 is the
#     initrd's ECM link to the Mac). /etc/apk/world forbids the kernels and those packages.
#     deviceinfo is a plain file (no flash method).
#   sb-integrate.sh NAME openrc (user "user" 10000, PIN build/linux/NAME-login.txt, SSH key, usb0 +
#     udhcpd for the Mac, GPU floor, AFBC env, scale 3, Wi-Fi firmware, reboot-to, sshd + keys)
#   greetd autologin of "user" into the UI's session with the XDG desktop variables phrog would set
#     (phrog stays the greeter after a logout); empty lib/modules/<release> for the modules bind
#   checks inside the image, then e2fsck -f, resize2fs -M, file truncated to the filesystem, sha256
# Output: IMG (default $P/build/distros-build/NAME.img) + IMG.sha256.
#
# Disk: the image and its temp dir (removed after) live in BUILD; apk runs with --no-cache and
# apk.static, the shim and the payload sit in a RAM tmpfs inside the mounted root (removed after).
# Before each big step it waits (does not fail) while the Mac has less than MINFREE MiB free.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
U=${SUDO_USER:-$(id -un)}
UH=$(getent passwd "$U" | cut -d: -f6)
export P=${P:-/Users/dev/project-starbite}
UI=${1:-phosh}
NAME=${NAME:-pmos}
BUILD=${BUILD:-$P/build/distros-build}
IMG=${IMG:-$BUILD/$NAME.img}
T=${T:-$BUILD/tmp-$NAME}                            # TMPDIR for anything that wants one
SIZE=${SIZE:-5G}
MINFREE=${MINFREE:-5120}                            # MiB free on the Mac's disk to go on
APKV=${APKV:-}                                      # apk-tools-static version (default: edge's)
APK_SHA=${APK_SHA:-}                                # optional pin of the .apk's sha256
A=${ALPINE:-https://dl-cdn.alpinelinux.org/alpine/edge}
A2=${ALPINE2:-https://mirrors.edge.kernel.org/alpine/edge}
PM=${PMOS:-https://mirror.postmarketos.org/postmarketos/master}
KEYS=$P/build/linux/keys

[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 1; }
# its own mount namespace, private: the VM's / is shared, so mounts under /mnt would otherwise be
# copied into the system services' namespaces (and keep the loop device busy after the umount) and
# the /dev/pts bind would stack onto the VM's /dev/pts and the other builds' chroots
[ -n "${SB_NS:-}" ] || SB_NS=1 exec unshare -m --propagation private bash "$0" "$@"
[ "$(uname -m)" = aarch64 ] || { echo "needs an arm64 host (the Lima VM)" >&2; exit 1; }
ls "$KEYS"/*.rsa.pub >/dev/null 2>&1 || { echo "no keys in $KEYS (pmbootstrap pmb/data/keys)" >&2; exit 1; }
mkdir -p "$BUILD" "$T"
export TMPDIR=$T
# the Mac's free space (the virtiofs mount reports the APFS volume): wait while it is low
room() {
	while a=$(df -BM --output=avail "$BUILD" | tail -1 | tr -dc 0-9); [ "$a" -lt "$MINFREE" ]; do
		echo "$(date +%T) $1: Mac has ${a} MiB free (< $MINFREE), waiting" >&2; sleep 60
	done
}

R=/mnt/sb-$NAME
S=$R/tmp/sb                                         # tmpfs: apk.static, shim, payload
LOOP=
cleanup() {
	set +e
	if mountpoint -q "$R"; then umount -R "$R" || umount -R -l "$R"; fi
	[ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null
	rm -rf "$T"
}
trap cleanup EXIT
trap 'echo "mkroot-pmos: failed at line $LINENO: $BASH_COMMAND" >&2' ERR
mkdir -p "$R"

# ---- the image, sparse on the Mac (virtiofs: a file made a moment ago may not be visible yet) ---
rm -f "$IMG" "$IMG.sha256"
room image
truncate -s "$SIZE" "$IMG"
mk() { mkfs.ext4 -q -F -E nodiscard -O ^orphan_file,^metadata_csum_seed -L "sb-$NAME" -m 0 "$IMG"; }
mk 2>/dev/null || { sleep 3; mk; }
LOOP=$(losetup -f --show "$IMG")
mount -o noatime "$LOOP" "$R"
mkdir -p "$S" "$R/dev" "$R/proc" "$R/sys" "$R/run" "$R/etc/apk/keys"
mount -t tmpfs -o size=256m tmpfs "$S"
mount --bind /dev "$R/dev"; mkdir -p "$R/dev/pts"; mount --bind /dev/pts "$R/dev/pts"
mount -t proc proc "$R/proc"; mount -t sysfs sys "$R/sys"; mount -t tmpfs tmpfs "$R/run"

# ---- apk.static: two HTTPS mirrors agree, and Alpine's signature on the binary ------------------
if [ -z "$APKV" ]; then
	curl -fsSL -o "$S/APKINDEX.tar.gz" "$A/main/aarch64/APKINDEX.tar.gz"
	APKV=$(tar -xzOf "$S/APKINDEX.tar.gz" APKINDEX 2>/dev/null |
		awk '/^P:apk-tools-static$/ {f=1} f && /^V:/ && !v {v=substr($0, 3)} END {print v}')
fi
[ -n "$APKV" ] || { echo "apk-tools-static: no version in the edge index" >&2; exit 1; }
F=apk-tools-static-$APKV.apk
curl -fsSL -o "$S/$F" "$A/main/aarch64/$F"
curl -fsSL -o "$S/$F.2" "$A2/main/aarch64/$F"
sha=$(sha256sum "$S/$F" | cut -c1-64)
[ "$sha" = "$(sha256sum "$S/$F.2" | cut -c1-64)" ] || { echo "$F differs between $A and $A2" >&2; exit 1; }
[ -z "$APK_SHA" ] || [ "$sha" = "$APK_SHA" ] || { echo "$F: sha256 $sha, expected $APK_SHA" >&2; exit 1; }
mkdir -p "$S/apk"
tar -xzf "$S/$F" -C "$S/apk" 2>/dev/null
APK=$S/apk/sbin/apk.static
sig=$(ls "$S"/apk/sbin/apk.static.SIGN.RSA.sha256.*.rsa.pub)
key=$KEYS/${sig##*.SIGN.RSA.sha256.}
[ -s "$key" ] || { echo "apk.static is signed by $(basename "$key"), not in $KEYS" >&2; exit 1; }
openssl dgst -sha256 -verify "$key" -signature "$sig" "$APK" >/dev/null || { echo "apk.static: bad signature" >&2; exit 1; }
echo "apk.static $APKV: sha256 $sha (two mirrors), signature ok ($(basename "$key"))"

cp "$KEYS"/*.rsa.pub "$R/etc/apk/keys/"
printf '%s\n' "$PM" "$A/main" "$A/community" "$A/testing" > "$R/etc/apk/repositories"
apk() { "$APK" --root "$R" --no-cache --no-interactive --no-progress "$@"; }

# ---- the shim: stands in for the initramfs generator and usb-moded ------------------------------
mkdir -p "$S/shim/usr/share/starbite"
cat > "$S/shim/usr/share/starbite/pmos-shim.txt" <<'EOF'
Starbite (tools/linux/distros/mkroot-pmos.sh): this package provides postmarketos-ramdisk and
postmarketos-usb-moded so that pmOS installs no initramfs generator, no boot-deploy and no usb-moded.
The kernel, its modules and the initrd come from Starbite (UFS store + /distros/_kernel on the card);
usb0 is the Starbite initrd's CDC-ECM link (172.16.42.1), which usb-moded would reconfigure.
EOF
"$APK" mkpkg --info name:starbite-pmos-shim --info version:1-r0 --info arch:noarch \
	--info "description:Starbite: no pmOS initramfs, boot-deploy or usb-moded" --info license:MIT \
	--info "provides:postmarketos-ramdisk postmarketos-usb-moded" \
	--files "$S/shim" --output "$S/starbite-pmos-shim.apk"
# alone and with no repositories: nothing else is ever installed unchecked
apk add --initdb --allow-untrusted --repositories-file /dev/null "$S/starbite-pmos-shim.apk"

# ---- postmarketOS ----------------------------------------------------------------------------
NEVER="!postmarketos-initramfs !postmarketos-mkinitfs !boot-deploy !usb-moded !usb-signaller
	!linux-lts !linux-virt !linux-edge !linux-stable !linux-postmarketos-exynos9810 !u-boot-tools
	!device-samsung-starlte !device-samsung-star2lte"
PKGS="apk-tools postmarketos-base postmarketos-base-nosplash postmarketos-ui-$UI
	postmarketos-base-ui-wifi-wpa_supplicant doas doas-sudo-shim openssh-server
	mesa-dri-gallium mesa-egl wlr-randr evtest kmscube glmark2 e2fsprogs exfatprogs"
room packages
# unudhcpd and openrc first: postmarketos-base-openrc's post-install (sh -e) links
# init.d/unudhcpd.usb0 and rc-update fails on the dangling link when unudhcpd comes later, which
# skips the rest of it (sshd, local, zram, shutdown runlevel). unudhcpd.usb0 is taken out below.
# shellcheck disable=SC2086
apk add $NEVER openrc unudhcpd
# shellcheck disable=SC2086
apk add $NEVER $PKGS
apk info -e "postmarketos-ui-$UI" >/dev/null
# the on-screen keyboard's GSettings schemas: Alpine splits them into stevia-schemas, which nothing
# pulls in. Without them phosh-osk-stevia aborts ("Settings schema 'mobi.phosh.osk' is not
# installed"), and sm.puri.OSK0 is a RequiredComponent of phosh.session: gnome-session gives up
# and shows its "something has gone wrong" screen behind the lock screen (first boot 2026-11-03).
if apk info -e stevia >/dev/null; then
	apk add stevia-schemas
	[ -f "$R/usr/share/glib-2.0/schemas/mobi.phosh.osk.gschema.xml" ] || { echo "no mobi.phosh.osk schema" >&2; exit 1; }
fi

# what pmOS tools read about the device (no flash method: nothing flashes, nothing deploys)
mkdir -p "$R/usr/share/deviceinfo"
cat > "$R/usr/share/deviceinfo/deviceinfo" <<'EOF'
# Starbite (mkroot-pmos.sh): no pmOS device package; kernel and boot chain are Starbite's
deviceinfo_format_version="0"
deviceinfo_name="Samsung Galaxy S9 (Project Starbite)"
deviceinfo_manufacturer="Samsung"
deviceinfo_codename="starbite-s9"
deviceinfo_year="2018"
deviceinfo_arch="aarch64"
deviceinfo_chassis="handset"
deviceinfo_keyboard="false"
deviceinfo_external_storage="true"
deviceinfo_screen_width="1440"
deviceinfo_screen_height="2960"
deviceinfo_dtb=""
deviceinfo_flash_method="none"
EOF

# ---- Starbite integration (shared) ------------------------------------------------------------
cp "$HERE/sb-integrate.sh" "$S/"
# mk-payload rewrites an older one-line login file into "user user" + "pin NNNNNN" (PIN kept)
UH=$UH sh "$HERE/mk-payload.sh" "$S/payload" "$NAME"
grep -q '^pin [0-9]\{6\}$' "$S/payload/login.txt" || { echo "no PIN for $NAME" >&2; exit 1; }
chroot "$R" /bin/sh /tmp/sb/sb-integrate.sh "$NAME" openrc /tmp/sb/payload

# ---- pmOS specifics -----------------------------------------------------------------------------
in_root() { chroot "$R" /usr/bin/env PATH=/usr/sbin:/usr/bin:/sbin:/bin "$@"; }
# the shell sb-integrate gave "user" must exist (bash is not in pmOS by default)
sh_=$(awk -F: '$1 == "user" {print $7}' "$R/etc/passwd")
[ -n "$sh_" ] && in_root test -x "$sh_" || sed -i 's|^\(user:[^:]*:10000:10000:[^:]*:[^:]*:\).*|\1/bin/ash|' "$R/etc/passwd"
for g in seat feedbackd; do in_root getent group "$g" >/dev/null && in_root addgroup user "$g" 2>/dev/null || true; done
# greetd: autologin of "user" into the UI's session on every boot (initial_session runs once per
# boot; after a logout greetd falls back to the default_session greeter, phrog). The UI package
# points greetd at its own config (conf.d/greetd cfgfile, e.g. /etc/phrog/greetd-config.toml): that
# file is copied to /etc/greetd/starbite.toml with the autologin added and conf.d points there, so
# a package upgrade of the greeter's config does not drop the autologin.
desk=$(ls "$R"/usr/share/wayland-sessions/*.desktop 2>/dev/null | head -1 || true)
case $UI in phosh) [ -f "$R/usr/share/wayland-sessions/phosh.desktop" ] && desk=$R/usr/share/wayland-sessions/phosh.desktop ;; esac
[ -n "$desk" ] || { echo "no wayland session for $UI" >&2; exit 1; }
SESSION_CMD=$(sed -n 's/^Exec=//p' "$desk" | head -1)
[ -n "$SESSION_CMD" ] || { echo "no Exec= in $desk" >&2; exit 1; }
SESSION_DESK=$(basename "$desk" .desktop)
SESSION_NAMES=$(sed -n 's/^DesktopNames=//p' "$desk" | head -1 | sed 's/;*$//; s/;/:/g')
SESSION_ENV="XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=${SESSION_NAMES:-$SESSION_DESK} XDG_SESSION_DESKTOP=$SESSION_DESK PAN_MAX_AFBC_PACKING_RATIO=0"
gcfg=$(sed -n 's/^[[:space:]]*cfgfile="\{0,1\}\([^"]*\)"\{0,1\}.*/\1/p' "$R/etc/conf.d/greetd" 2>/dev/null | tail -1)
[ -n "$gcfg" ] && [ "$gcfg" != /etc/greetd/starbite.toml ] && [ -f "$R$gcfg" ] || gcfg=/etc/greetd/config.toml
mkdir -p "$R/etc/greetd"
{
	if [ -f "$R$gcfg" ] && [ "$gcfg" != /etc/greetd/starbite.toml ]; then
		awk '/^#? *\[initial_session\]/ {skip=1; next} /^#? *\[/ {skip=0} !skip' "$R$gcfg"
	else
		printf '[terminal]\nvt = 7\n\n[default_session]\ncommand = "agreety --cmd /bin/sh"\nuser = "greetd"\n'
	fi
	# PAN_MAX_AFBC_PACKING_RATIO is in /etc/environment (pam_env), and on the command line too: the
	# autologin session is not authenticated, so it does not depend on pam_env's setcred hook.
	# The autologin bypasses phrog, which would export the desktop variables from the session file
	# (phosh-session sets none): XDG_CURRENT_DESKTOP from DesktopNames= (';' -> ':', e.g.
	# Phosh:GNOME, without which gnome-session drops the OnlyShowIn=Phosh; on-screen keyboard),
	# XDG_SESSION_DESKTOP from the file's name, XDG_SESSION_TYPE=wayland
	printf '\n# Starbite autologin (mkroot-pmos.sh): "user" (10000) straight into %s\n[initial_session]\ncommand = "env %s %s"\nuser = "user"\n' \
		"$SESSION_DESK" "$SESSION_ENV" "$SESSION_CMD"
} > "$R/etc/greetd/starbite.toml.new"
mv "$R/etc/greetd/starbite.toml.new" "$R/etc/greetd/starbite.toml"
touch "$R/etc/conf.d/greetd"
sed -i '/^[[:space:]]*cfgfile=/d' "$R/etc/conf.d/greetd"
printf '# Starbite (mkroot-pmos.sh): the UI greeter config (%s) plus the autologin of "user"\ncfgfile="/etc/greetd/starbite.toml"\n' "$gcfg" >> "$R/etc/conf.d/greetd"
in_root rc-update add greetd default >/dev/null
# nothing that touches the USB gadget, no modem, no hwclock (the initrd set the clock)
for s in usb-moded usb-signaller unudhcpd unudhcpd.usb0 swapfile modemmanager eg25-manager bluetooth hwclock swclock; do
	for l in boot default sysinit; do in_root rc-update del "$s" "$l" >/dev/null 2>&1 || true; done
done
for s in sshd local networkmanager elogind dbus; do
	[ -e "$R/etc/init.d/$s" ] && in_root rc-update add "$s" default >/dev/null 2>&1 || true
done
# NetworkManager never takes usb0 (sb-integrate wrote the keyfile rule; pmOS ships its own profile)
{ grep -rlsE '^interface-name=usb0' "$R/etc/NetworkManager/system-connections" "$R/usr/lib/NetworkManager/system-connections" || true; } |
	while read -r f; do echo "removed NM profile for usb0: ${f#"$R"}"; rm -f "$f"; done
# the shared modules image's mount point (the initrd binds /distros/_kernel/modules-REL.img there):
# with sb.rw=0 the root is read-only and the initrd cannot mkdir it, so it is made here, empty.
# Where the initrd's sb_mods_dir puts it: usr/lib/modules when /lib is a link, else lib/modules.
KREL=${KREL:-$(cat "$P/build/linux/release")}
[ -n "$KREL" ] || { echo "no kernel release in $P/build/linux/release" >&2; exit 1; }
if [ -L "$R/lib" ]; then MODDIR=usr/lib/modules/$KREL; else MODDIR=lib/modules/$KREL; fi
mkdir -p "$R/$MODDIR"
# machine-id: dbus makes it on the first boot (dbus-uuidgen --ensure), not baked into the image
rm -f "$R/etc/machine-id" "$R/var/lib/dbus/machine-id"
rm -f "$R/etc/ssh/ssh_host_"*
echo "ui=$UI session=$SESSION_CMD (greetd autologin)" >> "$R/etc/starbite/integrated"

# ---- checks inside the image ------------------------------------------------------------------
fail=0
c() { if ( cd "$R" && eval "$2" ) >/dev/null 2>&1; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }
c "os-release pmOS (Nura)"    "grep -qE '^ID=\"?(postmarketos|nura)\"?$' etc/os-release && chroot . apk info -e postmarketos-release"
c "no kernel package"         "! chroot . apk info 2>/dev/null | grep -E '^(linux-(lts|virt|edge|stable|postmarketos|rpi|[a-z0-9]+-[a-z0-9]+$)|device-|postmarketos-(mkinitfs|initramfs)|boot-deploy|u-boot|usb-moded|usb-signaller)' | grep -vqE '^(device-mapper|linux-(pam|headers|firmware|tools))'"
c "no kernel files"           "! ls boot/vmlinu* boot/Image* usr/lib/modules/*/kernel lib/modules/*/kernel 2>/dev/null | grep -q ."
c "world forbids kernels"     "grep -qx '!linux-lts' etc/apk/world && grep -qx '!postmarketos-mkinitfs' etc/apk/world"
c "openrc init"               "[ -e sbin/init ] && [ -x sbin/openrc ]"
c "user 10000:10000"          "grep -q '^user:x:10000:10000:' etc/passwd"
c "user shell exists"         "[ -x \".\$(grep '^user:' etc/passwd | cut -d: -f7)\" ]"
c "user groups"               "for g in video input audio netdev plugdev wheel; do grep -q \"^\$g:.*[:,]user\" etc/group || exit 1; done"
c "user has a password"       "grep '^user:' etc/shadow | cut -d: -f2 | grep -q '^\\$'"
c "root locked"               "grep '^root:' etc/shadow | cut -d: -f2 | grep -q '^!'"
c "doas for wheel"            "grep -qs 'permit persist :wheel' etc/doas.d/*.conf"
c "ssh key"                   "[ -s home/user/.ssh/authorized_keys ]"
c "sshd in default"           "[ -L etc/runlevels/default/sshd ] && [ -x usr/sbin/sshd ]"
c "no host keys baked in"     "! ls etc/ssh/ssh_host_* 2>/dev/null | grep -q ."
c "host keys made at boot"    "[ -x usr/local/sbin/sb-ssh-keys ] && grep -q sb-ssh-keys etc/local.d/50-starbite.start && grep -q 'ssh-keygen -A' etc/init.d/sshd"
c "sshd on every address"     "! grep -hs '^[[:space:]]*ListenAddress' etc/ssh/sshd_config etc/ssh/sshd_config.d/*.conf | grep -q ."
c "local.d in default"        "[ -L etc/runlevels/default/local ] && [ -x etc/local.d/50-starbite.start ]"
c "usb0 udhcpd for the Mac"   "grep -q 'start 172.16.42.2' etc/starbite/udhcpd.conf && grep -q udhcpd etc/local.d/50-starbite.start"
c "no gadget resetters"       "! ls etc/runlevels/*/usb-moded etc/runlevels/*/usb-signaller etc/runlevels/*/unudhcpd* etc/runlevels/*/modemmanager 2>/dev/null | grep -q ."
c "pmOS post-install complete" "[ -L etc/runlevels/shutdown/mount-ro ] && [ -L etc/runlevels/sysinit/udev ]"
c "NM leaves usb0 alone"      "grep -q interface-name:usb0 etc/NetworkManager/conf.d/90-starbite-usb0.conf"
c "networkmanager in default" "[ -L etc/runlevels/default/networkmanager ]"
c "greetd in default"         "[ -L etc/runlevels/default/greetd ]"
c "greetd autologin user"     "grep -qx 'cfgfile=\"/etc/greetd/starbite.toml\"' etc/conf.d/greetd && grep -A2 '^\[initial_session\]' etc/greetd/starbite.toml | grep -q 'user = \"user\"' && grep -q '^\[default_session\]' etc/greetd/starbite.toml"
c "autologin desktop env"     "grep -A1 '^\[initial_session\]' etc/greetd/starbite.toml | grep -q 'XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=[^ ]' && { [ \"$UI\" != phosh ] || grep -q 'XDG_CURRENT_DESKTOP=Phosh:GNOME XDG_SESSION_DESKTOP=phosh ' etc/greetd/starbite.toml; }"
c "modules mount point"       "[ -d '$MODDIR' ] && [ -z \"\$(ls -A '$MODDIR')\" ]"
c "session binary"         "[ -x usr/bin/phosh-session ] || [ \"$UI\" != phosh ]"
c "phosh scale 3"             "grep -A1 'output:DSI-1' etc/phosh/phoc.ini | grep -q 'scale = 3' || [ \"$UI\" != phosh ]"
c "AFBC env"                  "grep -qx PAN_MAX_AFBC_PACKING_RATIO=0 etc/environment"
c "GPU floor rule"            "grep -q 455000000 etc/udev/rules.d/90-starbite.rules"
c "Wi-Fi firmware"            "[ -s usr/lib/firmware/brcm/brcmfmac4361-pcie.bin ] || [ -s lib/firmware/brcm/brcmfmac4361-pcie.bin ]"
c "reboot-to + busybox"       "[ -x usr/local/sbin/reboot-to ] && [ -x usr/local/lib/starbite/busybox ]"
c "card dir root-only 0700"   "[ \"\$(stat -c %a:%u:%g var/lib/starbite)\" = 700:0:0 ]"
c "share mountpoint"          "[ -d media/starbite-sd/shared ]"
c "fstab has no root"         "! grep -v '^#' etc/fstab | grep -q ' / '"
c "no machine-id baked in"    "[ ! -s etc/machine-id ]"
c "mesa panfrost"             "ls usr/lib/dri/panfrost_dri.so usr/lib/libgallium-*.so 2>/dev/null | grep -q ."
grep -E '^(PRETTY_NAME|VERSION_ID)=' "$R/etc/os-release"
echo "packages: $(chroot "$R" apk info 2>/dev/null | wc -l)"
echo "runlevels: $(for l in sysinit boot default; do printf '%s: %s; ' "$l" "$(ls "$R/etc/runlevels/$l" | tr '\n' ' ')"; done)"
[ "$fail" = 0 ] || { echo "checks failed: image left at $IMG (not shrunk)" >&2; exit 1; }

# ---- unmount, check, shrink ---------------------------------------------------------------------
rm -rf "$R"/var/cache/apk/* "$R"/var/tmp/* "$R"/var/log/*.log
umount "$S"; rmdir "$S" 2>/dev/null || true
for m in "$R/dev/pts" "$R/dev" "$R/proc" "$R/sys" "$R/run"; do umount "$m"; done
used=$(df -BM --output=used "$R" | tail -1 | tr -dc 0-9)
room shrink
umount "$R"
e2fsck -fy "$LOOP" >/dev/null || [ $? -le 1 ]
resize2fs -M "$LOOP" >/dev/null 2>&1
e2fsck -fy "$LOOP" >/dev/null || [ $? -le 1 ]
bs=$(dumpe2fs -h "$LOOP" 2>/dev/null | awk -F: '/^Block size/ {print $2+0}')
blocks=$(dumpe2fs -h "$LOOP" 2>/dev/null | awk -F: '/^Block count/ {print $2+0}')
losetup -d "$LOOP"; LOOP=
truncate -s $(( blocks * bs )) "$IMG"
(cd "$(dirname "$IMG")" && sha256sum "$(basename "$IMG")") > "$IMG.sha256"
echo "$IMG: $(( blocks * bs / 1048576 )) MiB ext4 (sb-$NAME), ${used} MiB of files"
echo "manifest: $NAME $(( blocks * bs / 1048576 ))M pmos-edge-$UI $(cut -d' ' -f1 "$IMG.sha256") $(date -I) 7.1.0-starbite"
