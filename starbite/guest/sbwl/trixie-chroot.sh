#!/bin/bash
# sbwl's Debian trixie arm64 build + test chroot in the Lima VM (aarch64 native):
#   limactl shell starbite -- /Users/dev/project-starbite/starbite/guest/sbwl/trixie-chroot.sh make
#       mmdebstrap trixie (wlroots 0.18 -dev, cairo, librsvg, GTK 3 + 4 -dev) with test apps (foot, galculator:
#       GTK3, gnome-calculator: GTK4) and a user "user" (uid 1000), as the desktop root will have
#   ... trixie-chroot.sh build        sbwl, popuptest, hostsim, the GTK test apps into $OUT (default $WORK/out)
#   ... trixie-chroot.sh test         build, then hostsim's script (hostsim.script) with sbwl: PASS / FAIL lines
#   ... trixie-chroot.sh run CMD...   any command inside (proc, dev, /dev/shm, /run, the repo and $WORK bound)
# WORK: /mnt/build/sbguest2/sbwl. The chroot: $WORK/trixie (~1.3 GB).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-/mnt/build/sbguest2/sbwl}
R=$WORK/trixie
OUT=${OUT:-$WORK/out}
REPO=$(cd "$here/../../.." && pwd)

mounts() {
    for d in proc sys dev dev/pts dev/shm run tmp "$REPO" "$WORK/out"; do
        [ -d "$R/${d#/}" ] || sudo mkdir -p "$R/${d#/}"
    done
    mountpoint -q "$R/proc" || sudo mount -t proc proc "$R/proc"
    mountpoint -q "$R/sys" || sudo mount --rbind /sys "$R/sys"
    mountpoint -q "$R/dev" || sudo mount --bind /dev "$R/dev"
    mountpoint -q "$R/dev/pts" || sudo mount -t devpts devpts "$R/dev/pts" -o newinstance,ptmxmode=0666
    mountpoint -q "$R/dev/shm" || sudo mount -t tmpfs shm "$R/dev/shm" -o mode=1777
    mountpoint -q "$R/run" || sudo mount -t tmpfs run "$R/run" -o mode=755
    mountpoint -q "$R/tmp" || sudo mount -t tmpfs tmp "$R/tmp" -o mode=1777
    mountpoint -q "$R$REPO" || sudo mount --bind "$REPO" "$R$REPO"
    mountpoint -q "$R$WORK/out" || sudo mount --bind "$WORK/out" "$R$WORK/out"
}

# the processes running inside this chroot (only those: the VM runs other agents' work)
chroot_kill() {
    for p in /proc/[0-9]*; do
        [ "$(sudo readlink "$p/root" 2>/dev/null)" = "$R" ] && sudo kill -9 "${p#/proc/}" 2>/dev/null
    done
    true
}

case "${1:-}" in
make)
    mkdir -p "$WORK/out"
    [ -d "$R/usr" ] || sudo mmdebstrap --variant=apt --aptopt='Apt::Install-Recommends "false"' \
        --include=build-essential,pkg-config,libwlroots-0.18-dev,wayland-protocols,libwayland-dev,libxkbcommon-dev,libpixman-1-dev,libcairo2-dev,librsvg2-dev,librsvg2-common,libgtk-3-dev,libgtk-4-dev,foot,galculator,gnome-calculator,hicolor-icon-theme,adwaita-icon-theme,fonts-dejavu-core,dbus-daemon,dbus,xkb-data,passwd,procps,python3-minimal,strace,file \
        trixie "$R" http://deb.debian.org/debian > "$WORK/mmdebstrap.log" 2>&1
    sudo chroot "$R" sh -c 'getent passwd user >/dev/null || useradd -m -u 1000 -s /bin/bash user'
    echo "trixie-chroot: $R ($(sudo du -sh "$R" | cut -f1))"
    ;;
build)
    mkdir -p "$OUT"
    mounts
    sudo chroot "$R" "$here/build.sh" "$OUT"
    ;;
test)
    mkdir -p "$OUT"
    mounts
    sudo chroot "$R" "$here/build.sh" "$OUT"
    # the test apps' .desktop files (the GTK test programs, a Terminal=true app, an app that ignores SIGTERM)
    sudo install -m 755 "$OUT/sbwl-gtk3test" "$OUT/sbwl-gtk4test" "$R/usr/local/bin/"
    sudo mkdir -p "$R/usr/local/share/applications"
    sudo cp "$here"/test-apps/*.desktop "$R/usr/local/share/applications/"
    "$here/install-rootfs.sh" "$R" "$OUT/sbwl"          # /usr/bin/sbwl, /etc/sbwl/system-apps (SYSTEM)
    chroot_kill
    sudo rm -f "$R"/tmp/sbwl-*.txt
    cd "$OUT"
    sudo chroot "$R" env -i PATH=/usr/local/bin:/usr/bin:/bin HOME=/root LANG=C.UTF-8 \
        "$OUT/hostsim" -o "$OUT/hs" -s "$here/hostsim.script" -- "$OUT/sbwl" 2>&1 | tee "$OUT/hostsim.log" | \
        grep -a 'PASS\|FAIL\|RESULT\|hostsim:'
    chroot_kill                                       # anything an aborted run left
    ;;
run)
    shift
    mounts
    sudo chroot "$R" "$@"
    ;;
*)
    sed -n '2,10p' "$0"
    exit 1
    ;;
esac
