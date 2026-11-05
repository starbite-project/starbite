#!/bin/bash
# build-ntfsprogs.sh [TAG] : static aarch64 ntfsresize / ntfsclone / ntfsinfo / ntfsfix for TWRP (TWRP 3.7.0
# ships mount.ntfs/fsck.ntfs/mkfs.ntfs but no ntfsresize). Phase 2 of docs/ANDROID-PIXEL.md shrinks the
# WINDOWS NTFS volume with them. Source: github.com/tuxera/ntfs-3g (official upstream), tag TAG.
# Lima; output ~/android/pixel/ntfsprogs-TAG/{ntfsresize,ntfsclone,ntfsinfo,ntfsfix} + SHA256SUMS.
set -euo pipefail
TAG=${1:-2026.9.28}
D=~/android/pixel/ntfsprogs-$TAG
mkdir -p "$D/src"; cd "$D/src"
[ -s ntfs-3g-$TAG.tar.gz ] || curl -fsSL -o ntfs-3g-$TAG.tar.gz "https://github.com/tuxera/ntfs-3g/archive/refs/tags/$TAG.tar.gz"
sha256sum ntfs-3g-$TAG.tar.gz
rm -rf ntfs-3g-$TAG; tar xzf ntfs-3g-$TAG.tar.gz; cd ntfs-3g-$TAG
[ -x configure ] || ./autogen.sh >/dev/null 2>&1
./configure --disable-shared --enable-static --disable-ntfs-3g --disable-crypto --without-uuid \
    --without-hd --disable-ldconfig --disable-mtab --enable-really-static >/dev/null
make -j8 LDFLAGS=-all-static >/dev/null 2>&1 || make -j8 >/dev/null
for t in ntfsresize ntfsclone ntfsinfo ntfsfix; do
  f=$(find ntfsprogs -maxdepth 2 -type f -name "$t" -perm -u+x | head -1)
  cp "$f" "$D/$t"; strip "$D/$t"
  file "$D/$t" | grep -q 'statically linked' || { echo "$t is not static"; exit 1; }
done
cd "$D"; sha256sum ntfsresize ntfsclone ntfsinfo ntfsfix | tee SHA256SUMS
./ntfsresize --version 2>&1 | head -1
