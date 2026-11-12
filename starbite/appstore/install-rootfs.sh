#!/bin/sh
# Track F: the App Store engine into a guest root (called by guest/rootfs/base-customize.sh ROOT, as root):
#   /usr/local/sbin/sbstore                         the engine (sbinit runs `sbstore daemon` instead of sbpkg)
#   /usr/share/starbite/appstore/catalog.json       the recipes (catalog/*.toml, checked), + icons/ (160 px .argb
#                                                   for the store table, .png for the launchers) when built
# Needs Python 3.11+ where it runs (tomllib). Nothing else of the root is touched.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
R=${1:?usage: install-rootfs.sh ROOT}
PY=${PYTHON:-python3}
mkdir -p "$R/usr/local/sbin" && cp "$here/sbstore.py" "$R/usr/local/sbin/sbstore" && chmod 0755 "$R/usr/local/sbin/sbstore"
"$PY" -I "$here/tools/catalog.py" build "$R/usr/share/starbite/appstore" >/dev/null
icons=${SBSTORE_ICONS:-$here/../../build/appstore-icons}
if [ -d "$icons" ]; then
    mkdir -p "$R/usr/share/starbite/appstore/icons"
    cp "$icons"/*.argb "$icons"/*.png "$R/usr/share/starbite/appstore/icons/" 2>/dev/null || true
fi
# the offline mirror (tools/mirror.py OUT; OUT/debs: .debs + Packages) for a guest without network
if [ -n "${SBSTORE_MIRROR_SRC:-}" ] && [ -d "$SBSTORE_MIRROR_SRC" ]; then
    mkdir -p "$R/usr/share/starbite/appstore/mirror"
    cp -a "$SBSTORE_MIRROR_SRC/." "$R/usr/share/starbite/appstore/mirror/"
    if [ -f "$R/usr/share/starbite/appstore/mirror/debs/Packages" ]; then
        mkdir -p "$R/etc/apt/sources.list.d"
        echo "deb [trusted=yes] file:/usr/share/starbite/appstore/mirror/debs ./" \
            > "$R/etc/apt/sources.list.d/sbstore-mirror.list"
    fi
fi
mkdir -p "$R/var/lib/sbstore" "$R/var/log/sbstore" "$R/opt/sbstore"
echo "install-rootfs: sbstore $("$PY" -I "$here/sbstore.py" version) + $(grep -c '"id"' "$R/usr/share/starbite/appstore/catalog.json") catalog entries into $R"
