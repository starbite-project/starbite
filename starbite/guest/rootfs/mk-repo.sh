#!/bin/bash
# repo.erofs: the App Store's offline Debian repository (docs/STARBITE-LINUX-APPS.md L6), sbblk dev 2, mounted on
# /repo in the guest. For every entry of store-list.tsv: the .debs it needs beyond base.erofs (downloaded from the
# Debian mirror in a throw-away overlay of the base), the flat repository index (Packages, Release: apt-ftparchive;
# sources line "deb [trusted=yes] file:/repo ./"), the store index (store/index.tsv) and a 160x160 icon per entry
# (store/icons/KEY.png + KEY.argb: premultiplied ARGB, what sbpkg copies into the icon pool).
#   limactl shell starbite -- /Users/dev/project-starbite/starbite/guest/rootfs/mk-repo.sh
# Needs $OUT/base.erofs (mk-base.sh) and the dev root (pkg/build.sh makes it). RAM figures come from
# store-ram.tsv (measure-ram.sh's result; entries without one get ram_mb 0, "unmeasured").
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/mnt/build/sbguest2/rootfs}
BASE=${BASE:-$OUT/base.erofs}     # the base image to build / test against
MIRROR=${MIRROR:-http://deb.debian.org/debian}
SUITE=${SUITE:-trixie}
LIST=${LIST:-$here/store-list.tsv}
RAM_TSV=${RAM_TSV:-$here/store-ram.tsv}
DEVROOT=${DEVROOT:-$OUT/devroot}
NAME=${NAME:-repo}                # NAME=repo-small LIST=store-list-small.tsv: a second, smaller repository
W=$OUT/$NAME-work
T=$OUT/$NAME-tree
M=$W/root

cleanup() {
    for m in "$M/pool" "$M/dev" "$M/proc" "$M" "$W/t" "$W/lower" "$DEVROOT/tmp/icons"; do
        mountpoint -q "$m" 2>/dev/null && sudo umount -l "$m"
    done
    return 0
}
trap cleanup EXIT
cleanup
mkdir -p "$W" "$T/pool" "$T/store/icons"
sudo rm -rf "$W"/{lower,t,root,x,closure}
mkdir -p "$W"/{lower,t,root,x,closure}

# ---- a throw-away root: base.erofs + tmpfs upper, Debian's mirror as its only source ----
sudo mount -o loop,ro -t erofs "$BASE" "$W/lower"
sudo mount -t tmpfs -o size=2g tmpfs "$W/t"
sudo mkdir -p "$W/t/upper" "$W/t/work"
sudo mount -t overlay overlay -o "lowerdir=$W/lower,upperdir=$W/t/upper,workdir=$W/t/work" "$M"
sudo mount --bind /dev "$M/dev"
sudo mount -t proc proc "$M/proc"
sudo mkdir -p "$M/pool" && sudo mount --bind "$T/pool" "$M/pool"
sudo rm -f "$M/etc/apt/sources.list.d/starbite-repo.list"
echo "deb $MIRROR $SUITE main" | sudo tee "$M/etc/apt/sources.list.d/net.list" >/dev/null
sudo cp /etc/resolv.conf "$M/etc/resolv.conf"
sudo chroot "$M" apt-get update -q >/dev/null
sudo mkdir -p "$M/pool/partial"

# ---- per entry: what it needs beyond the base (a simulation against the base's dpkg state), then download ----
: > "$W/wanted"
while IFS=$'\t' read -r key title summary cat feat apt measure; do
    [[ -z "$key" || "$key" == \#* ]] && continue
    # shellcheck disable=SC2086
    sudo chroot "$M" apt-get -s install --no-install-recommends $apt 2>&1 |
        sed -n 's/^Inst \([^ ]*\) \(\[[^]]*\] \)\?(\([^ ]*\) [^[]*\[\([^]]*\)\]).*$/\1 \3 \4/p' > "$W/closure/$key"
    if [ -s "$W/closure/$key" ]; then
        # shellcheck disable=SC2086
        sudo chroot "$M" apt-get -y -q --download-only --no-install-recommends -o Dir::Cache::Archives=/pool \
            install $apt > "$W/closure/$key.log" 2>&1 || { tail "$W/closure/$key.log"; exit 1; }
    fi
    cat "$W/closure/$key" >> "$W/wanted"
    echo "mk-repo: $key: $(wc -l < "$W/closure/$key") packages beyond the base"
done < "$LIST"
sudo rm -rf "$T/pool/partial" "$T/pool/lock"

# ---- the .deb behind every (package, version): drop old versions from earlier runs ----
deb_of() {  # name version arch -> pool file (apt's naming: the epoch's ':' as %3a)
    local v=${2//:/%3a}
    echo "$T/pool/${1}_${v}_${3}.deb"
}
sort -u "$W/wanted" > "$W/wanted.u"
: > "$W/keep"
while read -r n v a; do
    f=$(deb_of "$n" "$v" "$a")
    [ -f "$f" ] || { echo "mk-repo: missing $f" >&2; exit 1; }
    echo "$f" >> "$W/keep"
done < "$W/wanted.u"
for f in "$T"/pool/*.deb; do grep -qxF "$f" "$W/keep" || { echo "mk-repo: dropping old $(basename "$f")"; rm -f "$f"; }; done

# ---- sizes per entry: dl = the .debs, inst = their Installed-Size ----
declare -A RAM METHOD
if [ -f "$RAM_TSV" ]; then
    while IFS=$'\t' read -r k r m _; do [[ "$k" == \#* || -z "$k" ]] || { RAM[$k]=$r; METHOD[$k]=$m; }; done < "$RAM_TSV"
fi
{
    echo "# Starbite App Store index v1 (rootfs/mk-repo.sh, $(date -u +%F)); tab-separated, one entry per line:"
    echo "# key	title	summary	category	ram_mb	featured	dl_kb	inst_kb	apt packages	ram method"
} > "$W/index.tsv"
printf '%-22s %8s %8s %6s  %s\n' entry dl_MB inst_MB ram_MB packages > "$OUT/$NAME-report.txt"
while IFS=$'\t' read -r key title summary cat feat apt measure; do
    [[ -z "$key" || "$key" == \#* ]] && continue
    dl=0 inst=0
    while read -r n v a; do
        f=$(deb_of "$n" "$v" "$a")
        dl=$((dl + ($(stat -c %s "$f") + 1023) / 1024))
        inst=$((inst + $(dpkg-deb -f "$f" Installed-Size)))
    done < "$W/closure/$key"
    ram=${RAM[$key]:-0} how=${METHOD[$key]:-unmeasured}
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$key" "$title" "$summary" "$cat" "$ram" "$feat" "$dl" "$inst" \
        "$apt" "$how" >> "$W/index.tsv"
    printf '%-22s %8.1f %8.1f %6s  %s\n' "$key" "$(echo "$dl/1024" | bc -l)" "$(echo "$inst/1024" | bc -l)" "$ram" \
        "$(wc -l < "$W/closure/$key")" >> "$OUT/$NAME-report.txt"
done < "$LIST"
cp "$W/index.tsv" "$T/store/index.tsv"

# ---- the flat apt repository ----
(
    cd "$T"
    apt-ftparchive packages pool > Packages 2>/dev/null
    gzip -9nkf Packages
    apt-ftparchive -o APT::FTPArchive::Release::Origin=Starbite -o APT::FTPArchive::Release::Label=Starbite \
        -o APT::FTPArchive::Release::Suite=starbite -o APT::FTPArchive::Release::Codename="$SUITE" \
        -o APT::FTPArchive::Release::Architectures=arm64 release . > Release.new
    mv Release.new Release
)

# ---- icons: the entry's .desktop Icon=, from its packages' icon dirs or the base, rendered at 160x160 ----
while IFS=$'\t' read -r key title summary cat feat apt measure; do
    [[ -z "$key" || "$key" == \#* ]] && continue
    X=$W/x/$key
    mkdir -p "$X"
    while read -r n v a; do
        dpkg-deb --fsys-tarfile "$(deb_of "$n" "$v" "$a")" |
            tar -x -C "$X" --wildcards './usr/share/icons/*' './usr/share/pixmaps/*' './usr/share/applications/*' \
                './usr/share/*/icons/*' 2>/dev/null || true
    done < "$W/closure/$key"
done < "$LIST"
if [ ! -x "$DEVROOT/usr/bin/rsvg-convert" ] || ! sudo chroot "$DEVROOT" python3 -c 'import PIL' 2>/dev/null; then
    sudo cp /etc/resolv.conf "$DEVROOT/etc/resolv.conf"
    sudo chroot "$DEVROOT" sh -c 'apt-get update -q >/dev/null && apt-get install -y -q --no-install-recommends librsvg2-bin python3-pil >/dev/null'
fi
sudo mkdir -p "$DEVROOT/tmp/icons"
sudo mount --bind "$W" "$DEVROOT/tmp/icons"
sudo mkdir -p "$DEVROOT/tmp/icons/lower-copy"
sudo cp "$here/render-icons.py" "$LIST" "$DEVROOT/tmp/"
# the base's icons and .desktop files for the SYSTEM entries (copied: the devroot cannot see the loop mount)
sudo tar -C "$W/lower" -c usr/share/icons/hicolor usr/share/icons/Adwaita usr/share/pixmaps usr/share/applications \
    2>/dev/null | sudo tar -C "$DEVROOT/tmp/icons/lower-copy" -x
sudo chroot "$DEVROOT" python3 -I /tmp/render-icons.py /tmp/store-list.tsv /tmp/icons/x /tmp/icons/lower-copy \
    /tmp/icons/icons-out
sudo cp "$W"/icons-out/* "$T/store/icons/"
sudo rm -rf "$W/icons-out" "$W/lower-copy" "$W/x"
sudo umount "$DEVROOT/tmp/icons"

# ---- the image ----
cleanup
sudo rm -f "$OUT/$NAME.erofs"
sudo mkfs.erofs -zlz4hc,12 -C65536 -L sb-repo -U 5b5b5b5b-0000-4000-8000-000000000003 "$OUT/$NAME.erofs" "$T" \
    > "$OUT/mkfs-$NAME.log" 2>&1 || { tail "$OUT/mkfs-$NAME.log"; exit 1; }
sudo chown "$(id -u):$(id -g)" "$OUT/$NAME.erofs"
fsck.erofs "$OUT/$NAME.erofs" > "$OUT/fsck-$NAME.log" 2>&1 || { echo "mk-repo: fsck.erofs failed"; tail "$OUT/fsck-$NAME.log"; exit 1; }
(cd "$OUT" && sha256sum $NAME.erofs > $NAME.erofs.sha256)
sudo rm -rf "$W"
echo "mk-repo: $OUT/$NAME.erofs $(($(stat -c %s "$OUT/$NAME.erofs") / 1048576)) MB," \
    "$(ls "$T"/pool/*.deb | wc -l) .debs, $(grep -vc '^#' "$T/store/index.tsv") store entries; sizes in $OUT/$NAME-report.txt"
