#!/bin/bash
# The host test of sbblk's card path: libsb's fence with the card-file grant (guest-card.patch) and the exFAT
# resolver of guest-card.h, on a fake card (card-test-image.py). Run in the Lima VM (gcc):
#   limactl shell starbite -- /Users/dev/project-starbite/starbite/guest/blk/card-test.sh
# TREE=<a toolkit tree that already has the patch> tests that tree; otherwise REV (c29ca0d) is extracted to
# $WORK/card-test/tree and guest-card.patch applied there (git apply: it must apply cleanly). With mkfs.exfat and
# sudo it also checks the resolver against a volume made by the real tools (files copied by the kernel's exFAT).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
REV=${REV:-c29ca0d}
W=${WORK:-/mnt/build/sbguest2/blk}/card-test
mkdir -p "$W"
if [ -z "${TREE:-}" ]; then
    TREE=$W/tree
    rm -rf "$TREE"
    mkdir -p "$TREE"
    git -C "$root" archive "$REV" starbite/libsb starbite/utility/exfat.c starbite/utility/exfat.h starbite/guest/sbos |
        tar -x -C "$TREE"
    (cd "$TREE" && git apply --check "$here/guest-card.patch" && git apply "$here/guest-card.patch")
    echo "card-test: guest-card.patch applies to $REV"
fi
T=$TREE/starbite
python3 "$here/card-test-image.py" "$W/card.img" > "$W/card.h"
python3 "$here/card-test-image.py" "$W/card-u.img" --p1-start 2049 > "$W/card-u.h"
gcc -O1 -Wall -Wextra -Werror -Wno-unused-parameter -I"$T/libsb" -I"$T/utility" -I"$T/guest/sbos" -o "$W/card-test" \
    "$here/card-test.c" "$T/libsb/str.c" "$T/libsb/fence.c" "$T/utility/exfat.c"
"$W/card-test" "$W/card.img" "$W/card.h" "$W/card-u.img" "$W/card-u.h"
# cp7i: the phone glue's stepped offer (gcard_begin / gcard_tick / gcard_abort) and its proof cache
gcc -O1 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function -I"$T/libsb" -I"$T/utility" -I"$T/guest/sbos" \
    -o "$W/card-glue-test" "$here/card-glue-test.c" "$T/libsb/str.c" "$T/libsb/fence.c" "$T/utility/exfat.c"
"$W/card-glue-test" "$W/card.img" | grep -v "^  log:"
# the same resolver on a volume the real tools made
if command -v mkfs.exfat > /dev/null && sudo -n true 2> /dev/null; then
    gcc -O1 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function -I"$T/libsb" -I"$T/utility" -I"$T/guest/sbos" \
        -o "$W/card-real" "$here/card-real.c" "$T/utility/exfat.c"
    img=$W/real.img
    rm -f "$img"
    truncate -s 96M "$img"
    printf 'label: dos\nstart=2048, size=131072, type=7\nstart=133120, size=32768, type=da\n' | sfdisk -q "$img"
    loop=$(sudo losetup --show -f -P "$img")
    trap 'sudo umount "$W/mnt" 2>/dev/null || true; sudo losetup -d "$loop" 2>/dev/null || true' EXIT
    sudo mkfs.exfat -q -L STARBITE "${loop}p1" > /dev/null
    mkdir -p "$W/mnt"
    if sudo mount -t exfat -o uid=$(id -u) "${loop}p1" "$W/mnt" 2> /dev/null; then
        mkdir -p "$W/mnt/starbite/linux"
        head -c 3000000 /dev/urandom > "$W/base.bin"
        head -c 8388608 /dev/urandom > "$W/overlay.bin"
        # a fragmented file: two files grown in turns, then one of them removed
        for i in 1 2 3 4; do
            head -c 300000 /dev/urandom >> "$W/mnt/starbite/linux/a.tmp"
            head -c 300000 /dev/urandom >> "$W/mnt/starbite/linux/frag.img"
            sync
        done
        cp "$W/base.bin" "$W/mnt/starbite/linux/base.erofs"
        cp "$W/overlay.bin" "$W/mnt/starbite/linux/overlay.img"
        cp "$W/mnt/starbite/linux/frag.img" "$W/frag.bin"
        sync
        sudo umount "$W/mnt"
        sudo losetup -d "$loop"
        trap - EXIT
        "$W/card-real" "$img" "$W/base.bin" "$W/overlay.bin" "$W/frag.bin"
    else
        sudo losetup -d "$loop"
        trap - EXIT
        echo "card-test: this kernel has no exFAT: the real-volume check is skipped"
    fi
else
    echo "card-test: no mkfs.exfat or sudo: the real-volume check is skipped"
fi
