#!/bin/sh
# The offline mirror's apt part (runs inside tools/vm-chroot.sh's trixie root, which is close to the guest's base):
#   mirror-debs.sh OUT PKG...     -> OUT/debs/*.deb (what PKG... needs beyond the root) + OUT/debs/Packages
# install-rootfs.sh adds it as `deb [trusted=yes] file:/usr/share/starbite/appstore/mirror/debs ./`, so the
# source method's build deps (build-essential ...) install without network.
set -eu
OUT=$1; shift
mkdir -p "$OUT/debs"
command -v apt-ftparchive >/dev/null || apt-get install -y -q --no-install-recommends apt-utils >/dev/null
apt-get install --print-uris -qq --no-install-recommends "$@" | sed -n "s/^'\([^']*\)' \([^ ]*\) .*/\1 \2/p" |
while read -r url file; do
    [ -f "$OUT/debs/$file" ] || curl -fsSL -o "$OUT/debs/$file" "$url"
done
(cd "$OUT/debs" && apt-ftparchive packages . > Packages)
echo "mirror-debs: $(ls "$OUT/debs"/*.deb | wc -l) debs, $(du -sm "$OUT/debs" | cut -f1) MB for: $*"
