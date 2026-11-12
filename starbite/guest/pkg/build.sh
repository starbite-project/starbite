#!/bin/bash
# sbpkg (docs/STARBITE-LINUX-APPS.md L6): the guest's App Store daemon, built for Debian trixie arm64 in a trixie
# chroot (the dev root mk-base.sh's tree uses: $DEVROOT, made with mmdebstrap + build-essential if missing).
#   limactl shell starbite -- /Users/dev/project-starbite/starbite/guest/pkg/build.sh [OUTDIR]
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-/mnt/build/sbguest2/rootfs/sbpkg-build}
DEVROOT=${DEVROOT:-/mnt/build/sbguest2/rootfs/devroot}
if [ ! -x "$DEVROOT/usr/bin/gcc" ]; then
    sudo mmdebstrap --variant=apt --include=build-essential,dpkg-dev,apt-utils,ca-certificates,python3-minimal \
        --aptopt='Apt::Install-Recommends "false"' trixie "$DEVROOT" http://deb.debian.org/debian
fi
mkdir -p "$OUT"
W=$DEVROOT/tmp/sbpkg-src
sudo rm -rf "$W" && sudo mkdir -p "$W/pkg"
sudo cp "$here/sbpkg.c" "$W/pkg/" && sudo cp "$here/../sbshm.h" "$W/"
sudo chroot "$DEVROOT" gcc -O2 -g0 -Wall -Wextra -Wno-unused-parameter -std=gnu11 -D_FORTIFY_SOURCE=2 \
    -o /tmp/sbpkg-src/sbpkg /tmp/sbpkg-src/pkg/sbpkg.c
sudo install -m 0755 "$W/sbpkg" "$OUT/sbpkg"
sudo chown "$(id -u):$(id -g)" "$OUT/sbpkg"
echo "sbpkg: $OUT/sbpkg ($(stat -c %s "$OUT/sbpkg") bytes, trixie arm64)"
