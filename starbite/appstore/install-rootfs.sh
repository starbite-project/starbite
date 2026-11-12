#!/bin/sh
# Track F: the App Store engine into a guest root (called by guest/rootfs/base-customize.sh ROOT, as root):
#   /usr/local/sbin/sbstore                         the engine (sbinit runs `sbstore daemon` instead of sbpkg)
#   /usr/share/starbite/appstore/catalog.json       the recipes (catalog/*.toml, checked), + icons/ (160 px .argb
#                                                   for the store table, .png for the launchers) + keys/
# Needs Python 3.11+ where it runs (tomllib). Nothing else of the root is touched.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
R=${1:?usage: install-rootfs.sh ROOT}
PY=${PYTHON:-python3}
mkdir -p "$R/usr/local/sbin" && cp "$here/sbstore.py" "$R/usr/local/sbin/sbstore" && chmod 0755 "$R/usr/local/sbin/sbstore"
"$PY" -I -B "$here/tools/catalog.py" build "$R/usr/share/starbite/appstore" >/dev/null
# The offline mirror normally is NOT in the base: it is the card file appstore.erofs (tools/mk-appstore-erofs.sh),
# mounted at /srv/appstore; sbstore adds its debs/ as an apt source itself. SBSTORE_MIRROR_SRC=DIR copies one into
# the root instead (test roots).
if [ -n "${SBSTORE_MIRROR_SRC:-}" ] && [ -d "$SBSTORE_MIRROR_SRC" ]; then
    mkdir -p "$R/usr/share/starbite/appstore/mirror"
    cp -a "$SBSTORE_MIRROR_SRC/." "$R/usr/share/starbite/appstore/mirror/"
fi
mkdir -p "$R/var/lib/sbstore" "$R/var/log/sbstore" "$R/opt/sbstore"
echo "install-rootfs: sbstore $("$PY" -I -B "$here/sbstore.py" version) + $(grep -c '"id"' "$R/usr/share/starbite/appstore/catalog.json") catalog entries into $R"
