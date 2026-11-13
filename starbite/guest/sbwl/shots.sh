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
    print(d['id'] + ':' + ','.join(a) + ':' + d.get('icon', '-') + ':' + d.get('name', d['id']).replace(' ', '_'))
EOF
)
down() {
    sudo umount -R -l "$S/root" 2>/dev/null
    sudo rm -rf "$S/up" "$S/work"
}
for entry in $apps; do
    id=${entry%%:*}
    rest=${entry#*:}
    pkgs=$(echo "${rest%%:*}" | tr ',' ' ')
    rest=${rest#*:}
    icon=${rest%%:*}
    name=$(echo "${rest#*:}" | tr '_' ' ')
    down
    mkdir -p "$S/up" "$S/work" "$S/root"
    sudo mount -t overlay -o lowerdir="$BASE",upperdir="$S/up",workdir="$S/work" overlay "$S/root" || continue
    for d in proc sys dev repo hs out var/lib/apt/lists; do sudo mkdir -p "$S/root/$d"; done
    sudo mount -t proc proc "$S/root/proc"; sudo mount --rbind /sys "$S/root/sys"; sudo mount --rbind /dev "$S/root/dev"
    sudo mount -t tmpfs -o mode=0755 run "$S/root/run"; sudo mount -t tmpfs -o mode=1777 tmp "$S/root/tmp"
    # a terminal's PTY (foot): a devpts of its own, its ptmx over /dev/ptmx
    sudo mount -t devpts -o newinstance,ptmxmode=0666,mode=0620,gid=5 devpts "$S/root/dev/pts" &&
        sudo mount --bind "$S/root/dev/pts/ptmx" "$S/root/dev/ptmx"
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
    # extra packages an app needs to run here (shots.actions "id+: pkg ..."), e.g. a game's data
    extra=$(grep -E "^$id\+:" "$here/shots.actions" 2>/dev/null | cut -d: -f2-)
    pkgs="$pkgs $extra"
    echo "== $id: $pkgs"
    if ! sudo chroot "$S/root" sh -c "apt-get -q update >/dev/null 2>&1; DEBIAN_FRONTEND=noninteractive apt-get -q -y \
            --no-install-recommends install $pkgs >/tmp/apt.log 2>&1 || { tail -3 /tmp/apt.log; exit 1; }"; then
        echo "$id: not installed"; down; continue
    fi
    # Qt apps on Wayland need Qt's wayland platform plugin (only a Recommends of theirs: without it they abort with
    # "no Qt platform plugin could be initialized")
    sudo chroot "$S/root" sh -c 'q=; dpkg -s libqt6gui6 >/dev/null 2>&1 && q="$q qt6-wayland";
        dpkg -s libqt5gui5 >/dev/null 2>&1 && q="$q qtwayland5";
        [ -z "$q" ] || DEBIAN_FRONTEND=noninteractive apt-get -q -y --no-install-recommends install $q >/dev/null 2>&1'
    # the app's own .desktop: the one named as the catalog's icon (org.gnome.Papers, not its -previewer), else the
    # first that is shown in menus
    all=$(sudo chroot "$S/root" sh -c "for p in $pkgs; do dpkg -L \$p 2>/dev/null; done" |
        grep -E '^/usr/share/applications/[^/]+\.desktop$')
    desk=$(echo "$all" | grep -m1 "/$icon\.desktop\$")
    [ -n "$desk" ] || for f in $all; do
        sudo grep -qi '^NoDisplay=true' "$S/root$f" || { desk=$f; break; }
    done
    desk=$(basename "${desk:-none}" .desktop)
    [ "$desk" != none ] || { echo "$id: no .desktop file"; down; continue; }
    # the script: settle, shot 1, then this app's steps (shots.actions: "id: step; step; ...", "shot" dumps the next)
    {
        # Track F's carousel: 720 x 1280 (9:16): the apps laid out in that viewport, the dumps of its rows
        printf 'wait_hello 20000\nwait_catalog 30000\nviewport 1280 720\nlaunch %s\nwait_window %s 1 90000\nwait_frame 60000\nsleep 6000\n' \
            "$desk" "$desk"
        # "id: pre: ...": before shot 1 (a first-run dialog closed)
        grep -E "^$id: *pre:" "$here/shots.actions" 2>/dev/null | cut -d: -f3- | tr ';' '\n' | sed 's/^ *//; s/ *$//' | grep -v '^$'
        printf 'sleep 1500\ndump %s-1\n' "$id"
        printf '1\t%s\n' "$name" > "$OUT/$id.captions"
        n=2
        steps=$(grep -E "^$id:" "$here/shots.actions" 2>/dev/null | grep -vE "^$id: *pre:" | cut -d: -f2-)
        IFS=';'
        set -f                              # (steps like "type 1234*56": no file name expansion)
        for st in $steps; do
            st=$(echo "$st" | sed 's/^ *//; s/ *$//')
            [ -n "$st" ] || continue
            case "$st" in
            shot | "shot "*)
                printf 'sleep 2500\ndump %s-%d\n' "$id" "$n"
                cap=${st#shot}
                cap=$(echo "$cap" | sed 's/^ *//')
                printf '%d\t%s\n' "$n" "${cap:-$name}" >> "$OUT/$id.captions"
                n=$((n + 1)) ;;
            *) echo "$st" ;;
            esac
        done
        unset IFS
        set +f
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
