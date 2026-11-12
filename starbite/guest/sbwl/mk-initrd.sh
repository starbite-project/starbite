#!/bin/bash
# The guest initramfs for Linux apps (docs/STARBITE-LINUX-APPS.md, L1): run-qemu.sh's base (busybox, guestd,
# sbguest.ko against KOUT, init) + sbwl + the app (foot) + their shared libraries (ldd closure from the VM's Ubuntu
# 24.04 arm64), the xkb data (us keymap), DejaVu Sans Mono with a fontconfig setup, a passwd entry.
#   limactl shell starbite -- /Users/dev/project-starbite/starbite/guest/sbwl/mk-initrd.sh
# Writes $WORK/out/initrd-wl.cpio.gz (reproducible: fixed mtimes, gzip -n).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-/mnt/build/sbguest}
KOUT=${KOUT:-$WORK/gout}
"$here/build.sh" "$WORK/sbwl" >/dev/null
BUILD_ONLY=1 WORK="$WORK" KOUT="$KOUT" "$here/../run-qemu.sh" wl > "$WORK/out/mk-initrd-wl.log" 2>&1 ||
    { tail -20 "$WORK/out/mk-initrd-wl.log"; exit 1; }
R=$WORK/rd-wl
rm -rf "$R"
cp -a "$WORK/rd" "$R"
cp "$WORK/sbwl/sbwl" "$R/sbwl"
mkdir -p "$R/usr/bin" "$R/etc" "$R/run" "$R/root" "$R/tmp"
cp /usr/bin/foot "$R/usr/bin/foot"
cp "$WORK/sbwl/popuptest" "$R/usr/bin/popuptest"         # sbwl.app=popuptest: the popup check
mkdir -p "$R/sbin" && rm -f "$R/sbin/e2fsck" && cp /usr/sbin/e2fsck "$R/sbin/e2fsck"   # init: the overlay checked first
# the shared libraries, at their own paths, and the loader
for f in $( (ldd "$R/sbwl"; ldd /usr/bin/foot; ldd "$R/usr/bin/popuptest"; ldd /usr/sbin/e2fsck) | awk '/=> \//{print $3} /^\t\/lib/{print $1}' | sort -u); do
    mkdir -p "$R$(dirname "$f")"
    cp -L "$f" "$R$f"
done
mkdir -p "$R/usr/lib/locale"
cp -a /usr/lib/locale/C.utf8 "$R/usr/lib/locale/"          # LC_ALL=C.UTF-8 (foot wants a UTF-8 locale)
mkdir -p "$R/usr/share/X11"
cp -a /usr/share/X11/xkb "$R/usr/share/X11/"
mkdir -p "$R/usr/share/fonts/truetype/dejavu"
cp /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf /usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf \
    "$R/usr/share/fonts/truetype/dejavu/"
cp -a /etc/fonts "$R/etc/"
mkdir -p "$R/usr/share/terminfo/f"
cp /usr/share/terminfo/f/foot* "$R/usr/share/terminfo/f/" 2>/dev/null || true
echo 'root:x:0:0:root:/root:/bin/sh' > "$R/etc/passwd"
echo 'root:x:0:' > "$R/etc/group"
printf '[main]\nfont=DejaVu Sans Mono:size=20\ndpi-aware=no\n[csd]\npreferred=none\n' > "$R/etc/foot.ini"
mkdir -p "$R/etc/xdg/foot" && cp "$R/etc/foot.ini" "$R/etc/xdg/foot/foot.ini"
find "$R" -exec touch -h -d @0 {} +
(cd "$R" && find . | sort | cpio -o -H newc --quiet --owner=0:0 --reproducible | gzip -9 -n) > "$WORK/out/initrd-wl.cpio.gz"
echo "mk-initrd: $(du -sh "$R" | cut -f1) unpacked, $(stat -c %s "$WORK/out/initrd-wl.cpio.gz") bytes gz," \
     "sha256 $(sha256sum "$WORK/out/initrd-wl.cpio.gz" | cut -c1-16)"
