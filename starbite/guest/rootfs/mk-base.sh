#!/bin/bash
# base.erofs: the guest's read-only root (docs/STARBITE-LINUX-APPS.md L4): Debian trixie arm64 without systemd,
# GTK 3 + GTK 4, fonts, icons, foot and a few small GNOME apps that make a usable phone, /sbin/sbinit as PID 1,
# sbpkg (the App Store daemon) and, optionally, sbwl. Run in the Lima VM (aarch64: no emulation):
#   limactl shell starbite -- /Users/dev/project-starbite/starbite/guest/rootfs/mk-base.sh
#   ... SBWL=/path/to/sbwl mk-base.sh      also install the compositor (sbwl/install-rootfs.sh: /usr/bin/sbwl)
#   ... KEEP_ROOT=1 mk-base.sh             keep the unpacked tree ($OUT/base-root) after making the image
#   ... REFRESH=1 KEEP_ROOT=1 mk-base.sh   reuse the kept tree: new sbinit / sbpkg / sbwl only, then the image
# Writes $OUT/$IMG.erofs (IMG=base) (EROFS, lz4hc, label sb-base), $OUT/base-packages.txt and $OUT/base-sizes.txt.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/mnt/build/sbguest2/rootfs}
MIRROR=${MIRROR:-http://deb.debian.org/debian}
SUITE=${SUITE:-trixie}
SBWL=${SBWL:-/mnt/build/sbguest2/sbwl/out/sbwl}     # empty: no compositor in the image
[ -z "$SBWL" ] || [ -f "$SBWL" ] || { echo "mk-base: no $SBWL (sbwl/trixie-chroot.sh build)" >&2; exit 1; }
SBPKG=${SBPKG:-}                  # a prebuilt sbpkg; default: build it (../pkg/build.sh in the dev chroot)
IMG=${IMG:-base}                   # the image's name: $OUT/$IMG.erofs (new builds under new names: IMG=base-v3)
ROOT=$OUT/base-root
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1762819200}    # 2025-11-11: stable timestamps in the image
mkdir -p "$OUT"

# The system apps (SYSTEM in the store: cannot be removed), picked for what they pull in: Nautilus would bring
# gvfs-backends (samba), localsearch (tracker, GStreamer) and udisks2 (~150 MB): it is in the store instead;
# Evince needs ghostscript (libspectre, ~50 MB with its fonts): Papers (its GTK 4 / libadwaita successor) does not;
# eog needs libpeas + Python and libgnome-desktop (iso-codes): gpicview is GTK 3 and tiny.
APPS=(
    gnome-text-editor          # Text Editor (GTK 4, libadwaita: adaptive)
    gnome-calculator           # Calculator (GTK 4, libadwaita)
    pcmanfm                    # Files (GTK 3, libfm)
    gpicview                   # Image Viewer (GTK 3)
    papers                     # Document Viewer: PDF (GTK 4, libadwaita)
)
PKGS=(
    # the system: apt for the store, no systemd (sbinit is PID 1)
    passwd mount util-linux procps kmod iproute2 ca-certificates less nano
    dbus-daemon dbus-bin dbus-session-bus-common dconf-gsettings-backend gsettings-desktop-schemas
    xdg-utils desktop-file-utils shared-mime-info libglib2.0-bin gtk-update-icon-cache
    # fonts: DejaVu (the terminal's mono) and Inter (the UI)
    fontconfig fonts-dejavu-core fonts-inter
    # toolkits and their look
    libgtk-3-0t64 libgtk-4-1 libadwaita-1-0 adwaita-icon-theme hicolor-icon-theme librsvg2-common
    libgdk-pixbuf2.0-bin gnome-themes-extra-data
    # the compositor's libraries (sbwl on wlroots 0.18) and the terminal
    libwlroots-0.18 libxkbcommon0 xkb-data foot foot-terminfo
    "${APPS[@]}"
)
# dpkg path excludes (also apply to what the store installs later: they stay in /etc/dpkg/dpkg.cfg.d)
EXCL=(
    path-exclude=/usr/share/doc/* path-include=/usr/share/doc/*/copyright
    path-exclude=/usr/share/man/* path-exclude=/usr/share/info/* path-exclude=/usr/share/help/*
    path-exclude=/usr/share/locale/* path-include=/usr/share/locale/locale.alias
    path-exclude=/usr/share/lintian/* path-exclude=/usr/share/linda/* path-exclude=/usr/share/gtk-doc/*
    path-exclude=/usr/share/bug/* path-exclude=/usr/share/gnome/help/*
)
dpkgopts=()
for e in "${EXCL[@]}"; do dpkgopts+=(--dpkgopt="$e"); done

if [ -z "$SBPKG" ]; then
    "$here/../pkg/build.sh" "$OUT/sbpkg-build" >/dev/null
    SBPKG=$OUT/sbpkg-build/sbpkg
fi

if [ "${REFRESH:-0}" = 1 ] && [ -x "$ROOT/sbin/sbinit" ]; then
    # REFRESH=1 (with a tree kept by KEEP_ROOT=1): only sbinit, apt.conf, the hidden .desktop files, sbpkg and sbwl change; no new packages
    sudo install -m 0755 "$here/sbinit" "$ROOT/sbin/sbinit"
    sudo install -m 0644 "$here/apt.conf" "$ROOT/etc/apt/apt.conf.d/90starbite"
    "$here/hide-desktop.sh" "$ROOT"
    sudo install -m 0755 "$SBPKG" "$ROOT/usr/local/sbin/sbpkg"
    [ -z "$SBWL" ] || sudo "$here/../sbwl/install-rootfs.sh" "$ROOT" "$SBWL" >/dev/null
    echo "mk-base: refreshed sbinit, sbpkg${SBWL:+, sbwl} in the kept tree"
else
sudo rm -rf "$ROOT"
mkdir -p "$OUT/debcache"
pkglist=$(IFS=,; echo "${PKGS[*]}")
# shellcheck disable=SC2016
sudo mmdebstrap --variant=apt --mode=root \
    --aptopt='APT::Install-Recommends "false"' --aptopt='APT::Install-Suggests "false"' \
    --aptopt='Acquire::Languages "none"' \
    "${dpkgopts[@]}" \
    --setup-hook='mkdir -p "$1/etc/apt/preferences.d"; printf "Package: systemd systemd-sysv libpam-systemd systemd-timesyncd\nPin: release *\nPin-Priority: -1\n" > "$1/etc/apt/preferences.d/no-systemd"' \
    --include="$pkglist" \
    --setup-hook='mkdir -p "$1/var/cache/apt/archives"' --setup-hook="sync-in $OUT/debcache /var/cache/apt/archives/" \
    --customize-hook="sync-out /var/cache/apt/archives $OUT/debcache" \
    --customize-hook="$here/base-customize.sh \"\$1\" \"$here\" \"$SBPKG\" \"$SBWL\"" \
    "$SUITE" "$ROOT" "$MIRROR" > "$OUT/mk-base.log" 2>&1 || { tail -30 "$OUT/mk-base.log"; exit 1; }

fi
sudo cp "$ROOT/usr/share/starbite/base-packages.txt" "$OUT/base-packages.txt"
# what takes the space (unpacked), for the report
{
    echo "unpacked: $(sudo du -sm "$ROOT" | cut -f1) MB"
    echo "--- biggest packages (Installed-Size, MB) ---"
    sudo chroot "$ROOT" dpkg-query -W -f='${Installed-Size}\t${Package}\n' | sort -rn | head -25 |
        awk '{printf "%7.1f  %s\n", $1/1024, $2}' || true
    echo "--- biggest directories (MB) ---"
    sudo du -m --max-depth=3 "$ROOT/usr" | sort -rn | head -20 | sed "s|$ROOT||" || true
} > "$OUT/base-sizes.txt"

sudo rm -f "$OUT/$IMG.erofs"
sudo mkfs.erofs -zlz4hc,12 -C65536 -Efragments,dedupe -T"$SOURCE_DATE_EPOCH" -L sb-base \
    -U 5b5b5b5b-0000-4000-8000-000000000001 "$OUT/$IMG.erofs" "$ROOT" > "$OUT/mkfs-$IMG.log" 2>&1 ||
    { tail "$OUT/mkfs-$IMG.log"; exit 1; }
sudo chown "$(id -u):$(id -g)" "$OUT/$IMG.erofs"
fsck.erofs "$OUT/$IMG.erofs" > "$OUT/fsck-$IMG.log" 2>&1 || { echo "mk-base: fsck.erofs failed"; tail "$OUT/fsck-$IMG.log"; exit 1; }
(cd "$OUT" && sha256sum $IMG.erofs > $IMG.erofs.sha256)
[ "${KEEP_ROOT:-0}" = 1 ] || sudo rm -rf "$ROOT"
echo "mk-base: $OUT/$IMG.erofs $(($(stat -c %s "$OUT/$IMG.erofs") / 1048576)) MB," \
    "$(wc -l < "$OUT/base-packages.txt") packages; sizes in $OUT/base-sizes.txt"
