#!/bin/sh
# sbwl (docs/STARBITE-LINUX-APPS.md, L1): the guest's Wayland compositor, built in the Lima VM against Ubuntu 24.04's
# wlroots 0.17 (libwlroots-dev, wayland-protocols, libxkbcommon-dev).
#   limactl shell starbite -- /Users/dev/project-starbite/starbite/guest/sbwl/build.sh [OUT]
set -eu
here=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-/mnt/build/sbguest/sbwl}
mkdir -p "$OUT"
P=$(pkg-config --variable=pkgdatadir wayland-protocols)
wayland-scanner server-header "$P/stable/xdg-shell/xdg-shell.xml" "$OUT/xdg-shell-protocol.h"
gcc -O2 -Wall -Wextra -Wno-unused-parameter -DWLR_USE_UNSTABLE -I"$OUT" \
    $(pkg-config --cflags wlroots wayland-server xkbcommon pixman-1) \
    "$here/sbwl.c" -o "$OUT/sbwl" $(pkg-config --libs wlroots wayland-server xkbcommon pixman-1)
# popuptest: the L2 popup check's client
wayland-scanner client-header "$P/stable/xdg-shell/xdg-shell.xml" "$OUT/xdg-shell-client-protocol.h"
wayland-scanner private-code "$P/stable/xdg-shell/xdg-shell.xml" "$OUT/xdg-shell-protocol.c"
gcc -O2 -Wall -Wextra -I"$OUT" "$here/popuptest.c" "$OUT/xdg-shell-protocol.c" -o "$OUT/popuptest" \
    $(pkg-config --cflags --libs wayland-client)
echo "sbwl: $OUT/sbwl ($(stat -c %s "$OUT/sbwl") bytes), $OUT/popuptest"
