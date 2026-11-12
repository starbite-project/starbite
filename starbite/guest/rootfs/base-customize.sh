#!/bin/bash
# mmdebstrap's customize hook for mk-base.sh (runs as root on the VM, the new root in $1):
#   base-customize.sh ROOT ROOTFS_DIR SBPKG_BIN [SBWL_BIN]
set -euo pipefail
R=$1 here=$2 sbpkg=$3 sbwl=${4:-}
ch() { chroot "$R" "$@"; }

# ---- PID 1, the store daemon, the compositor hook ----
install -m 0755 "$here/sbinit" "$R/sbin/sbinit"
install -D -m 0755 "$sbpkg" "$R/usr/local/sbin/sbpkg"
# Track F's App Store engine: starbite/appstore/install-rootfs.sh ROOT installs it (/usr/local/sbin/sbstore + its
# library); sbinit then runs `sbstore daemon` instead of sbpkg
if [ -x "$here/../../appstore/install-rootfs.sh" ]; then
    "$here/../../appstore/install-rootfs.sh" "$R"
fi
mkdir -p "$R/usr/local/bin" "$R/etc/starbite" "$R/repo"
# the SBWL= hook: sbwl's own installer (/usr/bin/sbwl, /etc/sbwl/system-apps), run at the end (it lists the base's
# .desktop files)
# ---- the user (uid 1000), the XDG dirs, the home on the overlay ----
ch useradd -m -u 1000 -U -s /bin/bash -c "Starbite" user
for d in Documents Downloads Pictures Music Videos Desktop; do mkdir -p "$R/etc/skel/$d" "$R/home/user/$d"; done
mkdir -p "$R/etc/skel/.config" "$R/home/user/.config"
cat > "$R/etc/skel/.config/user-dirs.dirs" <<'EOF'
XDG_DESKTOP_DIR="$HOME/Desktop"
XDG_DOCUMENTS_DIR="$HOME/Documents"
XDG_DOWNLOAD_DIR="$HOME/Downloads"
XDG_MUSIC_DIR="$HOME/Music"
XDG_PICTURES_DIR="$HOME/Pictures"
XDG_VIDEOS_DIR="$HOME/Videos"
XDG_TEMPLATES_DIR="$HOME"
XDG_PUBLICSHARE_DIR="$HOME"
EOF
cp "$R/etc/skel/.config/user-dirs.dirs" "$R/home/user/.config/"
ch chown -R user:user /home/user
chmod 0755 "$R/home/user"

# ---- system identity, locale, environment ----
echo starbite > "$R/etc/hostname"
printf '127.0.0.1\tlocalhost\n127.0.1.1\tstarbite\n::1\tlocalhost ip6-localhost ip6-loopback\n' > "$R/etc/hosts"
echo 'LANG=C.UTF-8' > "$R/etc/default/locale"
: > "$R/etc/machine-id"                                         # sbinit makes one on the overlay at first boot
rm -f "$R/var/lib/dbus/machine-id"; ln -sf /etc/machine-id "$R/var/lib/dbus/machine-id"
printf '# the root is put together by the initramfs (base.erofs + overlay); /repo by sbinit\n' > "$R/etc/fstab"
: > "$R/etc/resolv.conf"

# ---- apt: only the App Store's offline repository (file:/repo); Debian's mirror comes with the network (L5) ----
mv "$R/etc/apt/sources.list" "$R/etc/apt/sources.list.d/debian.list.disabled" 2>/dev/null || true
rm -f "$R"/etc/apt/sources.list.d/*.sources
echo 'deb [trusted=yes] file:/repo ./' > "$R/etc/apt/sources.list.d/starbite-repo.list"
install -m 0644 "$here/apt.conf" "$R/etc/apt/apt.conf.d/90starbite"

# ---- the look: Inter for the UI, DejaVu Sans Mono for the terminal, Adwaita icons ----
for v in 3.0 4.0; do
    mkdir -p "$R/etc/gtk-$v"
    printf '[Settings]\ngtk-font-name=Inter 11\ngtk-icon-theme-name=Adwaita\ngtk-application-prefer-dark-theme=0\ngtk-enable-animations=0\n' \
        > "$R/etc/gtk-$v/settings.ini"
done
cat > "$R/etc/fonts/local.conf" <<'EOF'
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
  <alias><family>sans-serif</family><prefer><family>Inter</family><family>DejaVu Sans</family></prefer></alias>
  <alias><family>monospace</family><prefer><family>DejaVu Sans Mono</family></prefer></alias>
  <alias><family>serif</family><prefer><family>DejaVu Serif</family></prefer></alias>
</fontconfig>
EOF
mkdir -p "$R/etc/xdg/foot"
printf '[main]\nfont=DejaVu Sans Mono:size=20\ndpi-aware=no\n[csd]\npreferred=none\n' > "$R/etc/xdg/foot/foot.ini"
# GNOME settings defaults: no animations, Inter
mkdir -p "$R/usr/share/glib-2.0/schemas"
cat > "$R/usr/share/glib-2.0/schemas/90-starbite.gschema.override" <<'EOF'
[org.gnome.desktop.interface]
font-name='Inter 11'
document-font-name='Inter 11'
monospace-font-name='DejaVu Sans Mono 11'
enable-animations=false
icon-theme='Adwaita'
EOF
ch glib-compile-schemas /usr/share/glib-2.0/schemas
ch fc-cache -f >/dev/null
ch gdk-pixbuf-query-loaders --update-cache 2>/dev/null || true
for t in hicolor Adwaita; do ch gtk-update-icon-cache -q -f "/usr/share/icons/$t" 2>/dev/null || true; done
ch update-desktop-database -q /usr/share/applications 2>/dev/null || true

# ---- what the base holds: sbpkg marks these store entries SYSTEM; all manual, so that no store removal's
# --autoremove can ever take a base package ----
mkdir -p "$R/usr/share/starbite"
ch sh -c 'dpkg-query -W -f="\${Package}\n" | xargs apt-mark manual >/dev/null'
ch dpkg-query -W -f='${Package}\n' | sort > "$R/usr/share/starbite/base-packages.txt"
date -u -d "@${SOURCE_DATE_EPOCH:-0}" +%F > "$R/usr/share/starbite/base-date"

# ---- the catalog: no helper entries on the home screen ----
"$here/hide-desktop.sh" "$R"

# ---- the compositor ----
if [ -n "$sbwl" ]; then
    "$here/../sbwl/install-rootfs.sh" "$R" "$sbwl"
    missing=$(ch ldd /usr/bin/sbwl | grep 'not found' || true)
    [ -z "$missing" ] || { echo "sbwl: libraries missing from the base: $missing" >&2; exit 1; }
fi

# ---- clean ----
ch apt-get clean
rm -rf "$R"/var/lib/apt/lists/* "$R"/var/cache/apt/*.bin "$R"/var/cache/debconf/*-old "$R"/var/lib/dpkg/*-old
find "$R/var/log" -type f -delete; rm -rf "$R"/tmp/* "$R"/var/tmp/* "$R"/root/.cache
find "$R/usr" -name '__pycache__' -prune -exec rm -rf {} + 2>/dev/null || true
