#!/bin/bash
# base-appstore.erofs: Track E's kept base tree (read-only lower, never written) + what the App Store needs, without a
# full mk-base.sh run (no re-download of the base): python3-minimal libpython3-stdlib curl xz-utils zstd unzip
# (what mk-base.sh now installs), sbstore + catalog + icons + the offline mirror (install-rootfs.sh), and the
# trunk's sbinit (starts `sbstore daemon`). Same mkfs.erofs options, label and UUID as mk-base.sh.
#   limactl shell starbite -- $PWD/starbite/appstore/tools/mk-base-appstore.sh
# LOWER (/mnt/build/sbguest2/rootfs/base-root), OUT (/mnt/build/appstore/out), IMG (base-appstore),
# MIRROR_SRC (the worktree's build/appstore-mirror; "" = none)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$here/.." && pwd)
REPO=$(cd "$TOP/../.." && pwd)
LOWER=${LOWER:-/mnt/build/sbguest2/rootfs/base-root}
OUT=${OUT:-/mnt/build/appstore/out}
IMG=${IMG:-base-appstore}
MIRROR_SRC=${MIRROR_SRC-$REPO/build/appstore-mirror}
W=/mnt/build/appstore/baseov
M=$W/m
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1762819200}
[ -x "$LOWER/sbin/sbinit" ] || { echo "no base tree at $LOWER"; exit 1; }
cleanup() {
    for d in proc dev/pts dev run tmp; do mountpoint -q "$M/$d" && sudo umount -l "$M/$d"; done
    mountpoint -q "$M" && sudo umount -l "$M"
    true
}
trap cleanup EXIT
cleanup
sudo rm -rf "$W"
sudo mkdir -p "$W"/{u,w,m} "$OUT"
sudo mount -t overlay ov -o lowerdir="$LOWER",upperdir="$W/u",workdir="$W/w" "$M"
sudo mount -t proc proc "$M/proc"; sudo mount --bind /dev "$M/dev"; sudo mount -t devpts devpts "$M/dev/pts"
sudo mount -t tmpfs run "$M/run"; sudo mount -t tmpfs tmp "$M/tmp"
sudo cp /etc/resolv.conf "$M/etc/resolv.conf.sbtmp"
sudo sh -c "cat '$M/etc/resolv.conf.sbtmp' > '$M/tmp/resolv.conf'"
sudo chroot "$M" sh -c '
    set -e
    mv /etc/resolv.conf /etc/resolv.conf.orig 2>/dev/null || true; cp /tmp/resolv.conf /etc/resolv.conf
    echo "deb [signed-by=/usr/share/keyrings/debian-archive-keyring.gpg] http://deb.debian.org/debian trixie main" > /etc/apt/sources.list.d/sbtmp.list
    mv /etc/apt/sources.list.d/starbite-repo.list /tmp/ 2>/dev/null || true
    apt-get update -q >/dev/null
    DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends python3-minimal libpython3-stdlib curl xz-utils zstd unzip >/dev/null
    rm -f /etc/apt/sources.list.d/sbtmp.list; mv /tmp/starbite-repo.list /etc/apt/sources.list.d/ 2>/dev/null || true
    apt-get clean; rm -rf /var/lib/apt/lists/*deb.debian.org*
    rm -f /etc/resolv.conf; mv /etc/resolv.conf.orig /etc/resolv.conf 2>/dev/null || true
    dpkg-query -W -f="\${Package}\n" | sort > /usr/share/starbite/base-packages.txt
    python3 -V'
sudo rm -f "$M/etc/resolv.conf.sbtmp"
sudo env SBSTORE_MIRROR_SRC="$MIRROR_SRC" sh "$TOP/install-rootfs.sh" "$M"
sudo install -m 0755 "$REPO/starbite/guest/rootfs/sbinit" "$M/sbin/sbinit"
cleanup
trap - EXIT
sudo mount -t overlay ov -o ro,lowerdir="$W/u:$LOWER" "$M"
sudo rm -f "$OUT/$IMG.erofs"
sudo mkfs.erofs -zlz4hc,12 -C65536 -Efragments,dedupe -T"$SOURCE_DATE_EPOCH" -L sb-base \
    -U 5b5b5b5b-0000-4000-8000-000000000001 "$OUT/$IMG.erofs" "$M" > "$OUT/mkfs-$IMG.log" 2>&1 ||
    { tail "$OUT/mkfs-$IMG.log"; sudo umount -l "$M"; exit 1; }
sudo umount -l "$M"
sudo chown "$(id -u):$(id -g)" "$OUT/$IMG.erofs"
fsck.erofs "$OUT/$IMG.erofs" > "$OUT/fsck-$IMG.log" 2>&1 || { echo "fsck.erofs failed"; tail "$OUT/fsck-$IMG.log"; exit 1; }
(cd "$OUT" && sha256sum "$IMG.erofs" > "$IMG.erofs.sha256")
sudo rm -rf "$W"
echo "mk-base-appstore: $OUT/$IMG.erofs $(($(stat -c %s "$OUT/$IMG.erofs") / 1048576)) MB $(cut -c1-16 "$OUT/$IMG.erofs.sha256")"
