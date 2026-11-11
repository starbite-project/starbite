#!/bin/bash
# The three Linux files for the phone's microSD card (exFAT partition p1, label STARBITE), and how they get there.
# Starbite OS serves them to the guest as block devices (starbite/guest/sbos/guest-card.h); nothing goes to UFS.
#
#   card-files.sh make OUT [BASE.erofs [REPO.erofs]]   (Lima VM) OUT/base.erofs, OUT/repo.erofs (copies) and
#       OUT/overlay.img: ext4, 4 KiB blocks, OVERLAY_SIZE (4G), label sb-overlay, with /upper and /work made;
#       OUT/SHA256SUMS. Without BASE the stand-in of mk-test-images.sh is used (a test root, not Debian).
#   card-files.sh push OUT        (Mac, the phone in TWRP, adb) the commands below, one by one, stopping at the
#       first that fails. WRITES THE CARD'S p1 (files only, through TWRP's exFAT). Run by the main session.
#   card-files.sh check OUT       (Mac, TWRP) read-only: p1 unmounted, then card-check.py --adb --verify OUT
#
# What push does (CARD = /dev/block/mmcblk0, MNT = /external_sd; change with CARD= / MNT=):
#   1. adb shell: mount p1 (twrp mount $MNT, else mount -t exfat ${CARD}p1 $MNT); it must be exFAT and p1
#   2. free space on it >= the three files + 64 MiB, else stop
#   3. mkdir -p $MNT/starbite/linux; old copies of the three files removed first (so each new file is laid down in
#      one piece in the free space, biggest first: overlay.img, base.erofs, repo.erofs), one adb push at a time
#   4. sync; sha256sum of each file on the phone against OUT/SHA256SUMS
#   5. umount $MNT (the directory entries and the FAT are on the card only after that)
#   6. check (below). A FAIL "FRAGMENTED" means the card's free space is in pieces: delete the three files and
#      push again after making room (the user's files on the card are the user's call), or pick a smaller
#      OVERLAY_SIZE. A FAIL "not on a 4 KiB boundary" for overlay.img means this card's exFAT layout is odd:
#      the files stay readable, the overlay cannot be written; say so and stop (reformatting p1 is the user's call).
# Nothing here touches p2 (the OS store), UFS, or anything outside /starbite/linux on p1.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
CARD=${CARD:-/dev/block/mmcblk0}
MNT=${MNT:-/external_sd}
cmd=${1:-}
OUT=${2:-}
[ -n "$cmd" ] && [ -n "$OUT" ] || { sed -n 2,26p "$0"; exit 2; }
files="overlay.img base.erofs repo.erofs"
case "$cmd" in
make)
    mkdir -p "$OUT"
    base=${3:-}
    repo=${4:-}
    if [ -z "$base" ]; then
        img=${WORK:-/mnt/build/sbguest2/blk}/img
        [ -f "$img/base.erofs" ] || OUT=$img "$here/mk-test-images.sh"
        base=$img/base.erofs
        repo=$img/repo.erofs
        echo "card-files: NO BASE GIVEN: using the test stand-in $base (busybox + the block tests, not the Debian root)"
    fi
    cp "$base" "$OUT/base.erofs"
    if [ -n "$repo" ]; then cp "$repo" "$OUT/repo.erofs"; else rm -f "$OUT/repo.erofs"; fi
    for f in base.erofs repo.erofs; do
        [ ! -f "$OUT/$f" ] || [ $(($(stat -c %s "$OUT/$f") % 4096)) = 0 ] || { echo "card-files: $f is not a multiple of 4 KiB"; exit 1; }
    done
    # the overlay: ext4 with 4 KiB blocks (sbblk's logical block size), upper/ and work/ ready for overlayfs
    rm -f "$OUT/overlay.img"
    truncate -s "${OVERLAY_SIZE:-4G}" "$OUT/overlay.img"
    mkfs.ext4 -q -F -b 4096 -L sb-overlay -m 1 -E lazy_itable_init=0,lazy_journal_init=0 "$OUT/overlay.img"
    printf 'mkdir upper\nmkdir work\n' | debugfs -w -f - "$OUT/overlay.img" > /dev/null 2>&1     # root:root 0755
    e2fsck -fn "$OUT/overlay.img" > /dev/null
    (cd "$OUT" && sha256sum $(for f in $files; do [ -f "$f" ] && echo "$f"; done) > SHA256SUMS)
    ls -la "$OUT"
    cat "$OUT/SHA256SUMS"
    ;;
push)
    [ -f "$OUT/SHA256SUMS" ] || { echo "card-files: no $OUT/SHA256SUMS (card-files.sh make)"; exit 1; }
    sh() { adb shell "$@"; }
    sh "ls ${CARD}p1" > /dev/null || { echo "card-files: no ${CARD}p1 on the phone (TWRP? the card in?)"; exit 1; }
    sh "grep -q ' $MNT ' /proc/mounts || twrp mount $MNT >/dev/null 2>&1 || mount -t exfat ${CARD}p1 $MNT"
    line=$(sh "grep ' $MNT ' /proc/mounts" | tr -d '\r')
    echo "card-files: $line"
    case "$line" in
    *"${CARD#/dev/block/}p1 "*) ;;          # the card's first partition (kernel exFAT or TWRP's fuse)
    *) echo "card-files: $MNT is not mounted from ${CARD}p1: stop"; exit 1 ;;
    esac
    need=0
    for f in $files; do [ -f "$OUT/$f" ] && need=$((need + $(stat -f %z "$OUT/$f" 2>/dev/null || stat -c %s "$OUT/$f"))); done
    sh "mkdir -p $MNT/starbite/linux && cd $MNT/starbite/linux && rm -f $files && sync"
    free_kb=$(sh "df -k $MNT | tail -1" | awk '{print $4}')
    echo "card-files: free on the card $((free_kb / 1024)) MiB, the files need $((need >> 20)) MiB"
    [ "$free_kb" -gt $(((need >> 10) + 65536)) ] || { echo "card-files: not enough room on p1: stop"; exit 1; }
    for f in $files; do
        [ -f "$OUT/$f" ] || continue
        adb push "$OUT/$f" "$MNT/starbite/linux/$f"
        sh sync
    done
    sh "cd $MNT/starbite/linux && sha256sum $(for f in $files; do [ -f "$OUT/$f" ] && printf '%s ' "$f"; done)" | tr -d '\r' |
        sort > "$OUT/SHA256SUMS.phone"
    if ! diff <(sort "$OUT/SHA256SUMS") "$OUT/SHA256SUMS.phone"; then
        echo "card-files: FAIL the files on the card do not match: stop"
        exit 1
    fi
    echo "card-files: the three files are on the card and match"
    sh "sync; umount $MNT"
    exec "$0" check "$OUT"
    ;;
check)
    if adb shell "grep -q '${CARD#/dev/block/}p1 ' /proc/mounts"; then
        echo "card-files: unmounting p1 first (its directory and FAT must be on the card)"
        adb shell "sync; umount $MNT"
    fi
    python3 "$here/card-check.py" --adb --dev "$CARD" --verify "$OUT" $(for f in base.erofs overlay.img repo.erofs; do [ -f "$OUT/$f" ] && echo "$f"; done)
    ;;
*)
    sed -n 2,26p "$0"
    exit 2
    ;;
esac
