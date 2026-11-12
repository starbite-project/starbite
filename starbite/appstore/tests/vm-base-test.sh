#!/bin/bash
# The guest's own software stack, offline, without QEMU: base-appstore.erofs + a fresh ext4 overlay + repo.erofs on
# /repo (as the initramfs mounts them), its python3 and /usr/local/sbin/sbstore, its catalog and mirror; the daemon
# against a plain file as /dev/sbshm and tests/vm-daemon-test.py as sbwl. No network (SBSTORE_OFFLINE=1, apt has
# only file: sources). PASS / FAIL lines.
#   limactl shell starbite -- $PWD/starbite/appstore/tests/vm-base-test.sh [ID:METHOD ...]
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$here/../../.." && pwd)
BASE=${BASE:-/mnt/build/appstore/out/base-appstore.erofs}
REPOIMG=${REPOIMG:-/mnt/build/sbguest2/rootfs/repo.erofs}
MIRRORIMG=${MIRRORIMG:-}                  # the card's appstore.erofs: mounted at /srv/appstore as on the phone
W=/mnt/build/appstore/basetest
M=$W/root
JOBS=${*:-firefox:distro neovim:release btop:source ripgrep:release}
cleanup() {
    for m in "$M/srv/appstore" "$W/mirror" "$M/tmp/t" "$M/repo" "$M/tmp" "$M/run" "$M/dev/pts" "$M/dev" "$M/proc" "$M" "$W/ov" "$W/repo" "$W/base"; do
        mountpoint -q "$m" 2>/dev/null && sudo umount -l "$m"
    done
    return 0
}
trap cleanup EXIT
cleanup
sudo rm -rf "$W"; sudo install -d -o "$(id -u)" -g "$(id -g)" "$W"; mkdir -p "$W"/{base,ov,repo,root,t,mirror}
"$REPO/starbite/guest/rootfs/mk-overlay.sh" "$W/overlay.img" 2G >/dev/null
sudo mount -o loop,ro -t erofs "$BASE" "$W/base"
sudo mount -o loop,ro -t erofs "$REPOIMG" "$W/repo"
sudo mount -o loop "$W/overlay.img" "$W/ov"
sudo mkdir -p "$W/ov/upper" "$W/ov/work"
sudo mount -t overlay root -o lowerdir="$W/base",upperdir="$W/ov/upper",workdir="$W/ov/work" "$M"
sudo mount -t proc proc "$M/proc"; sudo mount --bind /dev "$M/dev"; sudo mount -t devpts devpts "$M/dev/pts"
sudo mount -t tmpfs run "$M/run"; sudo mount -t tmpfs tmp "$M/tmp"
sudo mkdir -p "$M/repo" "$M/tmp/t"; sudo mount --bind "$W/repo" "$M/repo"
MIRROR=/usr/share/starbite/appstore/mirror
if [ -n "$MIRRORIMG" ]; then
    sudo mount -o loop,ro -t erofs "$MIRRORIMG" "$W/mirror"
    sudo mkdir -p "$M/srv/appstore"; sudo mount --bind "$W/mirror" "$M/srv/appstore"
    MIRROR=/srv/appstore
fi
sudo mkdir -p "$M/tmp/appstore/tests"; sudo cp "$here/../sbstore.py" "$M/tmp/appstore/sbstore.py"
sudo cp "$here/vm-daemon-test.py" "$M/tmp/appstore/tests/vm-daemon-test.py"
[ "${SBSTORE_NEW:-0}" = 1 ] && sudo install -m 755 "$here/../sbstore.py" "$M/usr/local/sbin/sbstore"   # this tree's engine
if [ "${CORRUPT_LISTS:-0}" = 1 ]; then    # cp3: an unclean stop left apt's saved index of /repo damaged
    sudo chroot "$M" sh -c 'apt-get -q update >/dev/null 2>&1; f=/var/lib/apt/lists/_repo_._Release; n=$(stat -c %s "$f"); { printf "%s\n" "-----BEGIN PGP SIGNED MESSAGE-----" "Hash: SHA512" ""; cat "$f"; } | head -c "$n" > "$f.x" && mv "$f.x" "$f" && touch -r /repo/Release "$f"; ls -la "$f"; apt-get -q update 2>&1 | tail -2; apt-get -s install nautilus 2>&1 | tail -2'
fi
echo "base $(sha256sum "$BASE" | cut -c1-16), repo $(sha256sum "$REPOIMG" | cut -c1-16), overlay fresh 2G (the phone's size)"
sudo chroot "$M" /usr/bin/env -i PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin HOME=/root LANG=C.UTF-8 \
    SBSTORE_OFFLINE=1 SBSTORE_BIN=/usr/local/sbin/sbstore SBSTORE_MIRROR=$MIRROR \
    SBSTORE_REPO=/repo python3 -I /tmp/appstore/tests/vm-daemon-test.py /usr/share/starbite/appstore/catalog.json $JOBS
sudo chroot "$M" sh -c 'python3 -V; ls /opt/sbstore 2>/dev/null; df -m / | tail -1'
