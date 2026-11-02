#!/usr/bin/env bash
# card-install.sh: put a distro image (or the shared modules image, or the phone-side build kit)
# onto the card's exFAT /distros/, from TWRP over adb. Runs on the Mac; the images stay in the VM
# and are streamed (no copy on the Mac's disk). docs/LINUX-REBUILD-PLAN.md 3.1.
#
#   card-install.sh image NAME [GROW]   VM ~/distros/NAME.img -> /distros/NAME.img, grown to GROW
#                                       (default 8G; "no" keeps the shipped size)
#   card-install.sh modules [FILE]      VM ~/distros/_kernel/modules-7.1.0-starbite.img (default,
#                                       from build-kernel.sh) -> /distros/_kernel/
#   card-install.sh kit                 sb-integrate.sh + payload (ubuntu login excluded) ->
#                                       /distros/sb-integrate/ for roots built on the phone
#   card-install.sh status              what is on the card, and free space
#
# The phone must be in TWRP with adb up (the user presses the buttons; this script only talks
# adb). The card's exFAT is TWRP's /external_sd; if TWRP has not mounted it, it is mounted from
# /dev/block/mmcblk0p1. Every copy goes to NAME.part first, is checked with sha256sum ON THE
# PHONE against the VM's, and only then renamed. Growing runs truncate + e2fsck + resize2fs on the
# phone when TWRP has resize2fs; otherwise the image is grown in the VM first (sparse there, but
# then the full size crosses USB). TWRP's 4.9 kernel mounts these images only ro,noload: all
# repairs after that run from the Starbite shell (linux-shell).
#
# The other way for big images: boot linux-shell, pick "Card to Mac" (or echo @card >
# /run/sb-boot), and copy onto the mounted STARBITE volume on the Mac.
set -euo pipefail
VM=${VM:-starbite}
VMHOME=${VMHOME:-/home/dev.guest}
KREL=${KREL:-$(limactl shell starbite -- bash -lc 'cat ~/distros/_kernel/release' 2>/dev/null || echo 7.1.0-starbite)}  # build-kernel.sh writes the release (with its build id)
HERE=$(cd "$(dirname "$0")" && pwd)
die() { echo "card-install: $*" >&2; exit 1; }
vm() { limactl shell "$VM" -- "$@"; }
ash() { adb shell "$@" | tr -d '\r'; }

phone_ready() {
	command -v adb >/dev/null || die "no adb on the Mac"
	[ "$(adb get-state 2>/dev/null)" = recovery ] || die "phone not in TWRP (adb get-state: $(adb get-state 2>&1 | head -1))"
	SD=
	if ash 'grep -q " /external_sd " /proc/mounts && echo y' | grep -q y; then
		SD=/external_sd
	else
		ash 'mkdir -p /external_sd && mount -t exfat -o rw,noatime /dev/block/mmcblk0p1 /external_sd 2>/dev/null ||
			mount -t sdfat -o rw,noatime /dev/block/mmcblk0p1 /external_sd' || die "could not mount the card's exFAT"
		SD=/external_sd
	fi
	ash "grep ' $SD ' /proc/mounts" | grep -q mmcblk0p1 || die "$SD is not the card's p1"
	ash "mkdir -p $SD/distros/_kernel $SD/shared"
}
# TWRP's busybox df cannot read the card's fuseblk (exfat-fuse) mount: stat -f -t gives
# name id namelen type bsize blocks bfree bavail ...
phone_free_mb() { ash "stat -f -t $SD" | awk '{print int($5 / 1024 * $8 / 1024)}'; }

# send VMFILE DEST: stream, verify on the phone, rename
send() {
	local src=$1 dest=$2 sha size free
	size=$(vm stat -c %s "$src") || die "no $src in the VM"
	sha=$(vm sha256sum "$src" | cut -d' ' -f1)
	free=$(phone_free_mb)
	[ $(( size / 1048576 + 256 )) -lt "$free" ] || die "$(( size / 1048576 )) MiB does not fit: $free MiB free on the card"
	echo "copying $(basename "$src") ($(( size / 1048576 )) MiB) to $dest ..."
	# adb exec-in drops data with TWRP's adbd (33.5 MB in, 33.1 MB on the phone, 2026-11-02):
	# stage the file on the Mac and adb push it (deleted after the push).
	local stage
	stage=$(mktemp "${TMPDIR:-/tmp}/card-install.XXXXXX")
	vm cat "$src" > "$stage"
	adb push "$stage" "$dest.part" >/dev/null || { rm -f "$stage"; die "adb push failed"; }
	rm -f "$stage"
	echo "checking sha256 on the phone ..."
	[ "$(ash "sha256sum '$dest.part'" | cut -d' ' -f1)" = "$sha" ] || { ash "rm -f '$dest.part'"; die "sha256 mismatch on $dest (removed)"; }
	ash "mv -f '$dest.part' '$dest' && sync"
	SENT_SHA=$sha
}

manifest() {                                        # name size source sha256 date kernel
	ash "grep -v '^$1 ' $SD/distros/manifest.txt > $SD/distros/manifest.new 2>/dev/null; \
		echo '$*' >> $SD/distros/manifest.new; mv -f $SD/distros/manifest.new $SD/distros/manifest.txt"
}

grow_size_bytes() {
	local g=$1
	case $g in *G) echo $(( ${g%G} << 30 )) ;; *M) echo $(( ${g%M} << 20 )) ;; *) die "size like 8G or 6144M" ;; esac
}

cmd=${1:-}
case $cmd in
image)
	NAME=${2:?card-install.sh image NAME [GROW]}
	GROW=${3:-8G}
	case $NAME in ""|_*|*[!a-z0-9-]*) die "NAME: lower case, digits, '-'" ;; esac
	SRC=$VMHOME/distros/$NAME.img
	phone_ready
	DEST=$SD/distros/$NAME.img
	if [ "$GROW" != no ] && ! ash 'command -v resize2fs >/dev/null && command -v e2fsck >/dev/null && echo y' | grep -q y; then
		echo "TWRP has no resize2fs: growing $NAME.img in the VM to $GROW first (sparse there)"
		vm sudo sh -c "truncate -s $GROW '$SRC' && e2fsck -fy '$SRC' >/dev/null; resize2fs '$SRC' >/dev/null && \
			cd \$(dirname '$SRC') && sha256sum $NAME.img > $NAME.img.sha256"
		GROW=no
	fi
	send "$SRC" "$DEST"
	if [ "$GROW" != no ]; then
		bytes=$(grow_size_bytes "$GROW")
		cur=$(ash "wc -c < '$DEST'")
		if [ "$bytes" -gt "$cur" ]; then
			free=$(phone_free_mb)
			[ $(( (bytes - cur) / 1048576 + 256 )) -lt "$free" ] || die "growing to $GROW needs $(( (bytes - cur) / 1048576 )) MiB: $free MiB free"
			echo "growing $NAME.img to $GROW on the phone ..."
			ash "truncate -s $bytes '$DEST' && e2fsck -fy '$DEST' >/dev/null; resize2fs '$DEST'" ||
				die "resize2fs failed: the image is still the shipped size and usable; grow it from linux-shell"
		fi
	fi
	manifest "$NAME" "$(( $(ash "wc -c < '$DEST'") / 1048576 ))M" "vm:$SRC" "sha:$SENT_SHA" "$(date -I)" "$KREL"
	echo "done: $DEST. Un-hide its catalog entry (tools/store/catalog.toml) if it is hidden."
	;;
modules)
	SRC=${2:-$VMHOME/distros/_kernel/modules-$KREL.img}
	phone_ready
	send "$SRC" "$SD/distros/_kernel/modules-$KREL.img"
	manifest "_kernel" "$(( $(vm stat -c %s "$SRC") / 1048576 ))M" "vm:$SRC" "sha:$SENT_SHA" "$(date -I)" "$KREL"
	echo "done: $SD/distros/_kernel/modules-$KREL.img"
	;;
kit)
	phone_ready
	# built in the VM (busybox and firmware live there), sent as one small tar (~3 MB)
	vm sh -c "rm -rf ~/distros/kit && mkdir -p ~/distros/kit && cp '$HERE/sb-integrate.sh' ~/distros/kit/ && \
		sh '$HERE/mk-payload.sh' ~/distros/kit/payload phone >/dev/null && rm -f ~/distros/kit/payload/login.txt && \
		tar -C ~/distros -cf ~/distros/kit.tar kit && rm -rf ~/distros/kit"
	ash "rm -rf $SD/distros/sb-integrate && mkdir -p $SD/distros/sb-integrate"
	vm cat "$VMHOME/distros/kit.tar" | adb exec-in "tar -xf - -C $SD/distros/sb-integrate --strip-components=1"
	vm rm -f "$VMHOME/distros/kit.tar"
	echo "done: $SD/distros/sb-integrate/ (sb-integrate.sh + payload; put a login.txt with 'pin NNNNNN' in payload/)"
	;;
status)
	phone_ready
	echo "free on the card: $(phone_free_mb) MiB"
	ash "ls -la $SD/distros $SD/distros/_kernel; cat $SD/distros/manifest.txt 2>/dev/null"
	;;
*)
	sed -n '2,24p' "$0"; exit 2 ;;
esac
