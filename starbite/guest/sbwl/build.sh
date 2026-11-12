#!/bin/sh
# sbwl (docs/STARBITE-LINUX-APPS.md, L1-L3): the guest's Wayland compositor. One source for two systems:
#   - the Lima VM's Ubuntu 24.04 (wlroots 0.17: libwlroots-dev, wayland-protocols, libxkbcommon-dev): the L1/L2
#     initramfs (mk-initrd.sh); no cairo/librsvg there, so no catalog icons (letter tiles need cairo too: none);
#     limactl shell starbite -- /Users/dev/project-starbite/starbite/guest/sbwl/build.sh [OUT]
#   - Debian trixie arm64 (wlroots 0.18: libwlroots-0.18-dev, libcairo2-dev, librsvg2-dev): the desktop root,
#     built inside the trixie chroot (trixie-chroot.sh build).
# Also builds popuptest (the L2 popup check's client), hostsim (the user-space host stand-in; PNG dumps with cairo),
# wltest (a plain Wayland client: keys, clipboard, spinning), and with GIO (libglib2.0-dev) sbnotify
# (../notify/sbnotify.c: org.freedesktop.Notifications -> sbwl, contract v3) and notifytest (its hostsim client).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-/mnt/build/sbguest/sbwl}
mkdir -p "$OUT"
WLR=wlroots
pkg-config --exists wlroots-0.18 && WLR=wlroots-0.18
ICONS="-DSBWL_NO_ICONS"
ICONLIBS=""
if pkg-config --exists cairo librsvg-2.0; then
    ICONS=$(pkg-config --cflags cairo librsvg-2.0)
    ICONLIBS=$(pkg-config --libs cairo librsvg-2.0)
fi
P=$(pkg-config --variable=pkgdatadir wayland-protocols)
wayland-scanner server-header "$P/stable/xdg-shell/xdg-shell.xml" "$OUT/xdg-shell-protocol.h"
# shellcheck disable=SC2086
gcc -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-format-truncation -DWLR_USE_UNSTABLE -I"$OUT" $ICONS \
    $(pkg-config --cflags $WLR wayland-server xkbcommon pixman-1 libdrm) \
    "$here/sbwl.c" -o "$OUT/sbwl" $(pkg-config --libs $WLR wayland-server xkbcommon pixman-1) $ICONLIBS
# popuptest: the L2 popup check's client
wayland-scanner client-header "$P/stable/xdg-shell/xdg-shell.xml" "$OUT/xdg-shell-client-protocol.h"
wayland-scanner private-code "$P/stable/xdg-shell/xdg-shell.xml" "$OUT/xdg-shell-protocol.c"
gcc -O2 -Wall -Wextra -I"$OUT" "$here/popuptest.c" "$OUT/xdg-shell-protocol.c" -o "$OUT/popuptest" \
    $(pkg-config --cflags --libs wayland-client)
gcc -O2 -Wall -Wextra -I"$OUT" "$here/wltest.c" "$OUT/xdg-shell-protocol.c" -o "$OUT/sbwl-wltest" \
    $(pkg-config --cflags --libs wayland-client)
extra=", $OUT/sbwl-wltest"
if pkg-config --exists gio-2.0 gio-unix-2.0; then
    # shellcheck disable=SC2046
    gcc -O2 -Wall -Wextra "$here/../notify/sbnotify.c" -o "$OUT/sbnotify" $(pkg-config --cflags --libs gio-2.0 gio-unix-2.0)
    # shellcheck disable=SC2046
    gcc -O2 -Wall -Wextra "$here/notifytest.c" -o "$OUT/sbwl-notifytest" $(pkg-config --cflags --libs gio-2.0)
    extra="$extra, $OUT/sbnotify, $OUT/sbwl-notifytest"
fi
if pkg-config --exists cairo; then
    # shellcheck disable=SC2046
    gcc -O2 -Wall -Wextra -Wno-format-truncation "$here/hostsim.c" -o "$OUT/hostsim" $(pkg-config --cflags --libs cairo)
else
    gcc -O2 -Wall -Wextra -Wno-format-truncation -DHOSTSIM_NO_PNG "$here/hostsim.c" -o "$OUT/hostsim"
fi
extra="$extra, $OUT/hostsim"
for g in gtk+-3.0:gtk3 gtk4:gtk4; do                 # hostsim's GTK test apps, where GTK is there
    if pkg-config --exists "${g%%:*}"; then
        # shellcheck disable=SC2046
        gcc -O2 -Wall "$here/gtktest.c" -o "$OUT/sbwl-${g#*:}test" $(pkg-config --cflags --libs "${g%%:*}")
        extra="$extra, $OUT/sbwl-${g#*:}test"
    fi
done
echo "sbwl ($WLR$([ -n "$ICONLIBS" ] && echo ', icons')): $OUT/sbwl ($(stat -c %s "$OUT/sbwl") bytes), $OUT/popuptest$extra"
