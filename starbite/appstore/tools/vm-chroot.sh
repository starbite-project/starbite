#!/bin/bash
# The App Store's own Debian trixie arm64 test root in the Lima VM (aarch64 native), close to the guest's base:
#   limactl shell starbite -- $PWD/starbite/appstore/tools/vm-chroot.sh make
#   ... vm-chroot.sh run CMD...      (proc, dev, /dev/shm, /run, /tmp and the repo bound; network from the VM)
#   ... vm-chroot.sh fresh NAME      a throwaway copy (overlayfs over the root) at $WORK/NAME for one test
#   ... vm-chroot.sh drop NAME
# WORK: /mnt/build/appstore. Never touches other sessions' chroots.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-/mnt/build/appstore}
R=${R:-$WORK/trixie}
REPO=$(cd "$here/../../.." && pwd)
# the guest base's own set (rootfs/mk-base.sh, without the GUI apps) + what sbstore needs
PKGS=passwd,mount,util-linux,procps,kmod,iproute2,ca-certificates,less,nano,dbus-daemon,xdg-utils,desktop-file-utils,shared-mime-info,libglib2.0-bin,gtk-update-icon-cache,fontconfig,fonts-dejavu-core,libgtk-3-0t64,libgtk-4-1,libadwaita-1-0,adwaita-icon-theme,hicolor-icon-theme,librsvg2-common,foot,python3-minimal,libpython3-stdlib,curl,xz-utils,zstd,bzip2,unzip,gpgv,file

mounts() {
    for d in proc dev dev/pts dev/shm run tmp "$REPO"; do sudo mkdir -p "$R/${d#/}"; done
    sudo mount --make-rprivate "$R" 2>/dev/null || true
    mountpoint -q "$R/proc" || sudo mount -t proc proc "$R/proc"
    # /dev is a SHARED mount in the VM: the chroot's copy is made a slave first, so the devpts / shm mounted on it
    # below never propagate back onto the VM's own /dev/pts and /dev/shm (or into other sessions' chroots)
    mountpoint -q "$R/dev" || { sudo mount --bind /dev "$R/dev" && sudo mount --make-rslave "$R/dev"; }
    mountpoint -q "$R/dev/pts" || sudo mount -t devpts devpts "$R/dev/pts" -o newinstance,ptmxmode=0666
    mountpoint -q "$R/dev/shm" || sudo mount -t tmpfs shm "$R/dev/shm" -o mode=1777
    mountpoint -q "$R/run" || sudo mount -t tmpfs run "$R/run" -o mode=755
    mountpoint -q "$R/tmp" || sudo mount -t tmpfs tmp "$R/tmp" -o mode=1777
    mountpoint -q "$R$REPO" || sudo mount --bind "$REPO" "$R$REPO"
    sudo cp /etc/resolv.conf "$R/etc/resolv.conf"
}
umounts() {
    for d in "$REPO" tmp run dev/shm dev/pts dev proc; do
        mountpoint -q "$R/${d#/}" && sudo umount -l "$R/${d#/}" || true
    done
}
case "${1:-}" in
make)
    sudo mkdir -p "$WORK"
    [ -d "$R/usr" ] || sudo mmdebstrap --variant=minbase --arch=arm64 --include="$PKGS" \
        --components=main trixie "$R" http://deb.debian.org/debian
    echo "deb http://deb.debian.org/debian-security trixie-security main" | sudo tee "$R/etc/apt/sources.list.d/security.list" >/dev/null
    sudo chroot "$R" sh -c 'id user >/dev/null 2>&1 || useradd -m -u 1000 -s /bin/bash user'
    mounts; sudo chroot "$R" apt-get update -q; echo "root: $R ($(sudo du -sh "$R" | cut -f1))"
    ;;
run)
    shift; mounts
    sudo chroot "$R" /usr/bin/env -i PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
        HOME=/root LANG=C.UTF-8 TERM=dumb "$@"
    ;;
fresh)
    # a throwaway root: overlayfs (lower = the root) at $WORK/$2, then `R=$WORK/$2/m vm-chroot.sh run ...`
    n=$2; sudo mkdir -p "$WORK/$n"/{u,w,m}
    mountpoint -q "$WORK/$n/m" || sudo mount -t overlay ov -o lowerdir="$R",upperdir="$WORK/$n/u",workdir="$WORK/$n/w" "$WORK/$n/m"
    echo "$WORK/$n/m"
    ;;
drop)
    n=$2; M=$WORK/$n/m; R=$M umounts; mountpoint -q "$M" && sudo umount -l "$M"; sudo rm -rf "${WORK:?}/$n"
    ;;
umount) umounts ;;
*) echo "usage: $0 make | run CMD... | fresh NAME | drop NAME | umount" >&2; exit 2 ;;
esac
