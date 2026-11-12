#!/bin/bash
# The offline mirror's .debs, resolved against the guest's REAL base (not a look-alike chroot): the base erofs with a
# throwaway tmpfs upper as the root, Debian's network source on, `apt-get install --print-uris PKG...`: exactly what
# the phone lacks. Then the proof, offline: only file:/repo + the new debs as sources, `apt-get install -s PKG...`
# must resolve. (mirror-debs.sh's chroot had bzip2; base-e4 has not: dpkg-dev, build-essential failed on the phone.)
#   limactl shell starbite -- $PWD/starbite/appstore/tools/mirror-debs-base.sh BASE.erofs REPO.erofs OUTDIR PKG...
#   -> OUTDIR/debs/*.deb + Packages (OUTDIR/debs is replaced)
set -euo pipefail
BASE=$1 REPOIMG=$2 OUT=$3; shift 3
PKGS="$*"
W=/mnt/build/appstore/debsbase
M=$W/root
cleanup() {
    for m in "$M/debs" "$M/repo" "$M/dev/pts" "$M/dev" "$M/proc" "$M" "$W/upper" "$W/repo" "$W/base"; do
        mountpoint -q "$m" 2>/dev/null && sudo umount -l "$m"
    done
    return 0
}
trap cleanup EXIT
cleanup
sudo rm -rf "$W"; sudo install -d -o "$(id -u)" -g "$(id -g)" "$W"; mkdir -p "$W"/{base,repo,upper,root,debs}
sudo mount -o loop,ro -t erofs "$BASE" "$W/base"
sudo mount -o loop,ro -t erofs "$REPOIMG" "$W/repo"
sudo mount -t tmpfs -o size=3g up "$W/upper"
sudo mkdir -p "$W/upper/u" "$W/upper/w"
sudo mount -t overlay root -o lowerdir="$W/base",upperdir="$W/upper/u",workdir="$W/upper/w" "$M"
sudo mount -t proc proc "$M/proc"; sudo mount --bind /dev "$M/dev"; sudo mount --make-rslave "$M/dev"
sudo mount -t devpts devpts "$M/dev/pts"
sudo mkdir -p "$M/repo" "$M/debs"; sudo mount --bind "$W/repo" "$M/repo"; sudo mount --bind "$W/debs" "$M/debs"
sudo cp -L /etc/resolv.conf "$M/etc/resolv.conf"
CH() { sudo chroot "$M" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME=/root LANG=C.UTF-8 \
       DEBIAN_FRONTEND=noninteractive "$@"; }
echo "base $(sha256sum "$BASE" | cut -c1-16), repo $(sha256sum "$REPOIMG" | cut -c1-16): $PKGS"
# 1. online: what the base lacks for PKGS (Debian's mirror + the base's own file:/repo, as on the phone online)
sudo cp "$M/etc/apt/sources.list.d/debian.list.disabled" "$M/etc/apt/sources.list.d/debian.list"
CH apt-get -q update >/dev/null
CH apt-get install --print-uris -qq --no-install-recommends $PKGS | sed -n "s/^'\([^']*\)' \([^ ]*\) .*/\1 \2/p" |
while read -r url file; do
    case $url in
        file:*) cp "${M}${url#file:}" "$W/debs/$file" ;;          # the base repo's own copy
        *) curl -fsSL -o "$W/debs/$file" "$url" ;;
    esac
done
CH sh -c 'command -v apt-ftparchive' >/dev/null || CH apt-get install -y -q --no-install-recommends apt-utils >/dev/null
CH sh -c 'cd /debs && apt-ftparchive packages . > Packages'
# 2. offline proof: the base's file:/repo + the new debs only (the phone with the card, no network)
sudo rm -f "$M/etc/apt/sources.list.d/debian.list"
echo 'deb [trusted=yes] file:/debs ./' | sudo tee "$M/etc/apt/sources.list.d/sbstore-mirror.list" >/dev/null
sudo rm -rf "$M/var/lib/apt/lists"/*; sudo mkdir -p "$M/var/lib/apt/lists/partial"
CH apt-get -q update >/dev/null
if CH apt-get install -s -q --no-install-recommends $PKGS > "$W/sim.txt" 2>&1; then
    echo "PASS offline: $(grep -c '^Inst ' "$W/sim.txt") packages resolve from file:/repo + the mirror's debs"
else
    tail -20 "$W/sim.txt"; echo "FAIL offline: $PKGS do not resolve"; exit 1
fi
rm -rf "$OUT/debs.new"; mkdir -p "$OUT/debs.new"
cp "$W/debs"/*.deb "$W/debs/Packages" "$OUT/debs.new/"
rm -rf "$OUT/debs"; mv "$OUT/debs.new" "$OUT/debs"
echo "mirror-debs-base: $(ls "$OUT/debs"/*.deb | wc -l) debs, $(du -sm "$OUT/debs" | cut -f1) MB -> $OUT/debs"
