#!/bin/bash
# appstore.erofs: the offline mirror as the card file /starbite/linux/appstore.erofs (Track E: block device 3, mounted
# read-only at /srv/appstore; absent = not offered). 4 KiB blocks, lz4hc, label sb-appstore.
#   limactl shell starbite -- $PWD/starbite/appstore/tools/mk-appstore-erofs.sh [MIRROR_DIR] [OUT.erofs]
# MIRROR_DIR: tools/mirror.py + tools/mirror-debs.sh output (default: the worktree's build/appstore-mirror).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$here/../../.." && pwd)
SRC=${1:-$REPO/build/appstore-mirror}
OUT=${2:-/mnt/build/appstore/out/appstore.erofs}
[ -f "$SRC/debs/Packages" ] || echo "note: $SRC has no debs/Packages (no offline build deps)"
mkdir -p "$(dirname "$OUT")"
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1762819200}
rm -f "$OUT"
mkfs.erofs -b 4096 -zlz4hc,12 -C65536 -T"$SOURCE_DATE_EPOCH" --all-root -L sb-appstore \
    -U 5b5b5b5b-0000-4000-8000-000000000004 "$OUT" "$SRC" > "$OUT.mkfs.log" 2>&1 || { tail "$OUT.mkfs.log"; exit 1; }
fsck.erofs "$OUT" > /dev/null
(cd "$(dirname "$OUT")" && sha256sum "$(basename "$OUT")" > "$(basename "$OUT").sha256")
rm -f "$OUT.mkfs.log"
echo "mk-appstore-erofs: $OUT $(stat -c %s "$OUT") bytes $(cut -c1-64 "$OUT.sha256")"
