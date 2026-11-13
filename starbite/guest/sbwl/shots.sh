#!/bin/bash
# The App Store's screenshots (Track F's product pages, A1): each GUI app of the catalog (starbite/appstore/catalog,
# launcher.terminal = false, installable from [distro] apt) installed into a throw-away overlay over the kept base
# tree (mk-base.sh KEEP_ROOT=1: $WORK/rootfs/base-root), started by sbwl with hostsim as the host (headless, the
# phone's 720 x 1344 surface), its window dumped as PNG: shot 1 after it settles, shots 2..3 after the scripted steps
# in shots.actions (keys, typing, taps). Then the overlay, its packages and downloads are deleted (the Mac's disk) and
# the VM's file system trimmed. Run in the Lima VM (root for the mounts, one app at a time):
#   limactl shell starbite -- flock /var/tmp/sbvm-heavy.lock .../starbite/guest/sbwl/shots.sh [ID...]
# Environment: WORK (/mnt/build/sbguest3), OUT ($WORK/shots/png), REPO (the card's repo.erofs, offline packages
# first), NET=0 (no Debian mirror). Writes OUT/<id>-<n>.png and OUT/<id>.log (the run's PASS / FAIL lines).
set -u
here=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-/mnt/build/sbguest3}
OUT=${OUT:-$WORK/shots/png}
REPO=${REPO:-/mnt/build/sbguest2/card/repo.erofs}
CAT=$here/../../appstore/catalog
HS=$WORK/desk/out/hostsim
BASE=$WORK/rootfs/base-root
S=$WORK/shots
mkdir -p "$OUT" "$S/lists"
[ -d "$BASE/usr" ] && [ -x "$HS" ] || { echo "shots: no $BASE or $HS (mk-base.sh KEEP_ROOT=1; trixie-chroot.sh build)"; exit 1; }

# the GUI apps: id, the apt packages, from the catalog
apps=$(python3 - "$CAT" "$@" <<'EOF'
import sys, tomllib, pathlib
cat, want = pathlib.Path(sys.argv[1]), set(sys.argv[2:])
for p in sorted(cat.glob('*.toml')):
    d = tomllib.load(open(p, 'rb'))
    l, a = d.get('launcher', {}), d.get('distro', {}).get('apt')
    if l.get('terminal') or not a or (want and d['id'] not in want):
        continue
    print(d['id'] + ':' + ','.join(a))
EOF
)
down() {
    sudo umount -R -l "$S/root" 2>/dev/null
    sudo rm -rf "$S/up" "$S/work"
}
for entry in $apps; do
    id=${entry%%:*}
    pkgs=$(echo "${entry#*:}" | tr ',' ' ')
    down
    mkdir -p "$S/up" "$S/work" "$S/root"
    sudo mount -t overlay -o lowerdir="$BASE",upperdir="$S/up",workdir="$S/work" overlay "$S/root" || continue
    for d in proc sys dev repo hs out var/lib/apt/lists; do sudo mkdir -p "$S/root/$d"; done
    sudo mount -t proc proc "$S/root/proc"; sudo mount --rbind /sys "$S/root/sys"; sudo mount --rbind /dev "$S/root/dev"
    sudo mount -t tmpfs -o mode=0755 run "$S/root/run"; sudo mount -t tmpfs -o mode=1777 tmp "$S/root/tmp"
    [ -f "$REPO" ] && sudo mount -o ro,loop -t erofs "$REPO" "$S/root/repo"
    sudo mount --bind "$S/lists" "$S/root/var/lib/apt/lists"       # (the lists shared between the apps)
    sudo cp "$HS" "$S/root/hs/hostsim"
    sudo mount --bind "$OUT" "$S/root/out"
    if [ "${NET:-1}" = 1 ]; then
        sudo cp /etc/resolv.conf "$S/root/etc/resolv.conf"
        echo 'deb http://deb.debian.org/debian trixie main' | sudo tee "$S/root/etc/apt/sources.list.d/debian.list" >/dev/null
    fi
    # the first-boot parts of sbinit: the user's home, the machine id
    sudo chroot "$S/root" sh -c 'mkdir -p /home/user && cp -a /etc/skel/. /home/user/ 2>/dev/null; chown -R user:user /home/user;
        [ -s /etc/machine-id ] || dbus-uuidgen > /etc/machine-id' 2>/dev/null
    echo "== $id: $pkgs"
    if ! sudo chroot "$S/root" sh -c "apt-get -q update >/dev/null 2>&1; DEBIAN_FRONTEND=noninteractive apt-get -q -y \
            --no-install-recommends install $pkgs >/tmp/apt.log 2>&1 || { tail -3 /tmp/apt.log; exit 1; }"; then
        echo "$id: not installed"; down; continue
    fi
    desk=$(sudo chroot "$S/root" sh -c "for p in $pkgs; do dpkg -L \$p 2>/dev/null; done" |
        grep -E '^/usr/share/applications/[^/]+\.desktop$' | head -1)
    desk=$(basename "${desk:-none}" .desktop)
    [ "$desk" != none ] || { echo "$id: no .desktop file"; down; continue; }
    # the script: settle, shot 1, then this app's steps (shots.actions: "id: step; step; ...", "shot" dumps the next)
    {
        printf 'wait_hello 20000\nwait_catalog 30000\nlaunch %s\nwait_window %s 1 90000\nwait_frame 60000\nsleep 6000\ndump %s-1\n' \
            "$desk" "$desk" "$id"
        n=2
        steps=$(grep -E "^$id:" "$here/shots.actions" 2>/dev/null | cut -d: -f2-)
        IFS=';'
        for st in $steps; do
            st=$(echo "$st" | sed 's/^ *//; s/ *$//')
            [ -n "$st" ] || continue
            if [ "$st" = shot ]; then printf 'sleep 2500\ndump %s-%d\n' "$id" "$n"; n=$((n + 1)); else echo "$st"; fi
        done
        unset IFS
        printf 'stop 8000\n'
    } > "$S/script"
    sudo cp "$S/script" "$S/root/hs/shot.script"
    sudo chroot "$S/root" /usr/bin/env -i PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/usr/games HOME=/root \
        LANG=C.UTF-8 GSK_RENDERER=cairo GDK_BACKEND=wayland NO_AT_BRIDGE=1 GTK_A11Y=none MOZ_ENABLE_WAYLAND=1 \
        LIBGL_ALWAYS_SOFTWARE=1 SAL_USE_VCLPLUGIN=gtk3 XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=Starbite \
        sh -c 'SBWL_SOCK=/run/sbwl.sock /hs/hostsim -o /out -s /hs/shot.script -- /usr/bin/sbwl' > "$S/run.log" 2>&1
    grep -aE '^(PASS|FAIL|RESULT)' "$S/run.log" > "$OUT/$id.log"
    grep -aq 'RESULT PASS' "$S/run.log" || sudo cp "$S/run.log" "$OUT/$id.run.log"   # (a failure's whole log)
    sudo rm -f "$OUT"/*.ppm "$OUT/shm.bin"
    echo "$id ($desk): $(grep -a RESULT "$S/run.log" | cut -c1-40); shots $(ls "$OUT/$id"-*.png 2>/dev/null | wc -l)"
    down
done
sudo rm -rf "$S/lists"/*_Packages* "$S/lists"/*_InRelease 2>/dev/null
sudo fstrim / >/dev/null 2>&1
ls -la "$OUT" | tail -n +2 | wc -l
