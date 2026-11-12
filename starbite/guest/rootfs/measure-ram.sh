#!/bin/bash
# Measure the store entries' RAM (store-ram.tsv) in the VM: a chroot of base.erofs with a tmpfs upper (nothing on
# disk), every store entry installed from repo.erofs (offline, as on the phone; this also proves the repo resolves),
# cage from Debian's mirror (wlroots 0.18 like sbwl: headless backend, pixman renderer, a seat without devices; the
# output set to 720x1440 with wlr-randr), then measure-ram.py; grim takes a screenshot of every app as proof it drew.
#   limactl shell starbite -- /Users/dev/project-starbite/starbite/guest/rootfs/measure-ram.sh [KEY...]
# Writes $OUT/store-ram.tsv (copy it to rootfs/store-ram.tsv and run mk-repo.sh again to put it in the index).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/mnt/build/sbguest2/rootfs}
REPO=${REPO:-$OUT/repo.erofs}
BASE=${BASE:-$OUT/base.erofs}     # the base image to build / test against
MIRROR=${MIRROR:-http://deb.debian.org/debian}
DEVROOT=${DEVROOT:-$OUT/devroot}
W=$OUT/measure
M=$W/root
cleanup() {
    sudo pkill -KILL -f "[c]age -- sleep 100000" 2>/dev/null || true; sudo pkill -KILL -x -f "[s]leep 100000" 2>/dev/null || true
    sudo pkill -f "[d]bus-daemon --session --address=unix:path=/run/user/1000/bus" 2>/dev/null || true
    for m in "$M/repo" "$M/run" "$M/dev/shm" "$M/dev/pts" "$M/dev" "$M/proc" "$M/sys" "$M" "$W/t" "$W/repo" "$W/lower"; do
        mountpoint -q "$m" 2>/dev/null && sudo umount -l "$m"
    done
    return 0
}
trap cleanup EXIT
cleanup
mkdir -p "$W"/{lower,t,root,repo}
sudo mount -o loop,ro -t erofs "$BASE" "$W/lower"
sudo mount -o loop,ro -t erofs "$REPO" "$W/repo"
sudo mount -t tmpfs -o size=5g tmpfs "$W/t"
sudo mkdir -p "$W/t/upper" "$W/t/work"
sudo mount -t overlay overlay -o "lowerdir=$W/lower,upperdir=$W/t/upper,workdir=$W/t/work" "$M"
sudo mount -t proc proc "$M/proc"
sudo mount -t sysfs sys "$M/sys"
sudo mount --bind /dev "$M/dev"
sudo mount -t devpts -o newinstance,ptmxmode=0666 devpts "$M/dev/pts"
sudo mount -t tmpfs -o mode=1777 shm "$M/dev/shm"
sudo mount -t tmpfs -o mode=0755 run "$M/run"
sudo mount --bind "$W/repo" "$M/repo"
ch() { sudo chroot "$M" "$@"; }

# every entry, offline from the repo
apts=$(awk -F'\t' '!/^#/ && NF>5 {print $6}' "$here/store-list.tsv" | tr ' ' '\n' | sort -u | tr '\n' ' ')
ch apt-get update -q >/dev/null
# shellcheck disable=SC2086
ch env DEBIAN_FRONTEND=noninteractive apt-get install -y -q $apts > "$W/install.log" 2>&1 ||
    { tail -20 "$W/install.log"; exit 1; }
echo "measure-ram: installed from the repo: $(echo $apts | wc -w) packages asked, upper now $(sudo du -sm "$W/t/upper" | cut -f1) MB"
# the measuring tools, from the mirror (not part of anything shipped)
sudo cp /etc/resolv.conf "$M/etc/resolv.conf"
echo "deb $MIRROR trixie main" | sudo tee "$M/etc/apt/sources.list.d/net.list" >/dev/null
ch apt-get update -q >/dev/null
ch env DEBIAN_FRONTEND=noninteractive apt-get install -y -q cage grim wlr-randr >> "$W/install.log" 2>&1

# documents: generated in the dev root (PIL), converted by LibreOffice itself
sudo mkdir -p "$DEVROOT/tmp/tf"
sudo cp "$here/gen-testfiles.py" "$DEVROOT/tmp/"
sudo chroot "$DEVROOT" python3 -I /tmp/gen-testfiles.py /tmp/tf
sudo mkdir -p "$M/home/user/Documents" "$M/home/user/Pictures"
sudo cp "$DEVROOT"/tmp/tf/{page.html,notes.md,sheet.csv,slides.fodp} "$M/home/user/Documents/"
sudo cp "$DEVROOT"/tmp/tf/{photo.png,photo-small.png,drawing.svg} "$M/home/user/Pictures/"
sudo rm -rf "$DEVROOT/tmp/tf"
sudo mkdir -p "$M/run/user/1000" && ch chown -R user:user /run/user/1000 /home/user && sudo chmod 700 "$M/run/user/1000"
asu() { ch setpriv --reuid=1000 --regid=1000 --init-groups env HOME=/home/user XDG_RUNTIME_DIR=/run/user/1000 "$@"; }
D=/home/user/Documents
# LibreOffice's own headless conversion: the documents, and its peak RSS as a second data point for Writer
command -v /usr/bin/time >/dev/null && /usr/bin/time -f "%M" -o "$W/lo-convert.kb" sudo chroot "$M" setpriv --reuid=1000 --regid=1000 --init-groups \
    env HOME=/home/user soffice --headless --convert-to odt:writer8 --outdir $D $D/page.html >/dev/null 2>&1 || true
asu sh -c "cd $D && mv page.odt doc.odt && soffice --headless --convert-to pdf doc.odt && soffice --headless --convert-to ods sheet.csv" >/dev/null 2>&1
ls -la "$M$D" | tail -n +4

# the session: D-Bus and cage (headless, pixman: software rendering like sbwl; the app full window like on the phone)
asu dbus-daemon --session --address=unix:path=/run/user/1000/bus --nofork --nopidfile </dev/null >/dev/null 2>&1 &
asu env WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 cage -- sleep 100000 \
    </dev/null > "$W/cage.log" 2>&1 &
for _ in $(seq 150); do sudo test -S "$M/run/user/1000/wayland-0" && break; sleep 0.2; done
if ! sudo test -S "$M/run/user/1000/wayland-0"; then
    echo "measure-ram: cage did not start"; tail -5 "$W/cage.log"; exit 1
fi
asu env WAYLAND_DISPLAY=wayland-0 wlr-randr --output HEADLESS-1 --custom-mode 720x1440 ||
    echo "measure-ram: could not set 720x1440 (cage's default output size stays)"
asu env WAYLAND_DISPLAY=wayland-0 wlr-randr | head -3
sudo mkdir -p "$M/tmp/shots" && sudo chmod 1777 "$M/tmp/shots"

sudo python3 -I "$here/measure-ram.py" "$M" "$here/store-list.tsv" "$W/store-ram.tsv" "$@"
echo "lo-headless-convert	$(( ($(cat "$W/lo-convert.kb" 2>/dev/null || echo 0) + 1023) / 1024 ))	peak RSS (time -f %M) of soffice --headless --convert-to odt of page.html" |
    sudo tee -a "$W/store-ram.tsv" >/dev/null
cp "$W/store-ram.tsv" "$OUT/store-ram.tsv"
[ "${HOLD:-0}" = 1 ] && { echo "measure-ram: holding the session (kill me to clean up)"; sleep 100000; }
mkdir -p "$OUT/shots" && sudo cp "$M"/tmp/shots/*.png "$OUT/shots/" 2>/dev/null && sudo chown -R "$(id -u)" "$OUT/shots"
echo "measure-ram: $OUT/store-ram.tsv"
