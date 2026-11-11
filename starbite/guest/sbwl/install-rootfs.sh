#!/bin/bash
# Install sbwl (the trixie build) into a Debian trixie arm64 desktop root (docs/STARBITE-LINUX-APPS.md L3-L4):
#   install-rootfs.sh ROOT [SBWL]     SBWL: default /mnt/build/sbguest2/sbwl/out/sbwl (trixie-chroot.sh build)
# - /usr/bin/sbwl (root runs it, as the guest's compositor; it starts the apps as "user" when the root has one)
# - /etc/sbwl/system-apps: the desktop ids of the base image's .desktop files (the catalog's SYSTEM flag: apps
#   installed later are not in it). Run this after the base image's packages are in, before the overlay exists.
# - checks the packages sbwl needs (apt pulls their dependencies):
#     libwlroots-0.18 libwayland-server0 libxkbcommon0 libpixman-1-0 libcairo2 librsvg2-2 libglib2.0-0t64
#     xkb-data (the us keymap), fonts-dejavu-core (letter-tile icons), hicolor-icon-theme adwaita-icon-theme
#   and, optional: foot (Terminal=true apps run inside it), dbus-daemon (a session bus for GApplication apps)
set -euo pipefail
ROOT=${1:?usage: install-rootfs.sh ROOT [SBWL]}
SBWL=${2:-/mnt/build/sbguest2/sbwl/out/sbwl}
SUDO=""
[ -w "$ROOT/usr/bin" ] || SUDO=sudo
$SUDO install -D -m 755 "$SBWL" "$ROOT/usr/bin/sbwl"
$SUDO mkdir -p "$ROOT/etc/sbwl"
(cd "$ROOT/usr/share/applications" 2>/dev/null && find . -name '*.desktop' | sed 's|^\./||; s|/|-|g; s|\.desktop$||' | sort) |
    $SUDO tee "$ROOT/etc/sbwl/system-apps" > /dev/null
echo "install-rootfs: $ROOT/usr/bin/sbwl, $(wc -l < "$ROOT/etc/sbwl/system-apps") system apps"
missing=""
for p in libwlroots-0.18 libwayland-server0 libxkbcommon0 libpixman-1-0 libcairo2 librsvg2-2 libglib2.0-0t64 xkb-data \
         fonts-dejavu-core hicolor-icon-theme adwaita-icon-theme foot dbus-daemon; do
    grep -q "^Package: $p\$" "$ROOT/var/lib/dpkg/status" 2>/dev/null || missing="$missing $p"
done
[ -z "$missing" ] && echo "install-rootfs: every package sbwl uses is in the root" ||
    echo "install-rootfs: not in the root:$missing"
