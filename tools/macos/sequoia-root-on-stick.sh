#!/usr/bin/env bash
# The Sequoia desktop root built inside an APFS sparse image on the USB stick (2026-11-11): a raw HFSX image made
# straight on the exFAT stick needs a 16 GiB zero-fill, and the stick hung / dropped off during it twice. Inside an
# APFS .sparseimage the 16 GiB file is sparse (no zero-fill); the image's bands grow only as data is written.
#   tools/macos/sequoia-root-on-stick.sh [STICK=/Volumes/SBWORK]
# Result: <work mount>/starbite-macos15/root-15desktop.img, the work image left ATTACHED at its private mount point
# (build/macos15/sb15work-mnt) for sequoia-post-image.sh and card-range-flash-fast.py; detach it later by that mount
# point only (never by /dev/diskN, never -force).
set -euo pipefail
cd "$(dirname "$0")/../.."
STICK=${1:-/Volumes/SBWORK}
SPARSE=$STICK/starbite-macos15-work.sparseimage
MNT=$PWD/build/macos15/sb15work-mnt
LOG=build/macos15/desktop-build3.log
mkdir -p build/macos15 "$MNT"
[ -d "$STICK" ] && touch "$STICK/.starbite-alive" || { echo "$STICK is not there or not writable"; exit 1; }
# the old raw image on exFAT (from the stalled runs) goes; keys.txt is kept
rm -f "$STICK/starbite-macos15/root-15desktop.img" "$STICK/starbite-macos15/._root-15desktop.img"
if [ ! -f "$SPARSE" ]; then
    hdiutil create -size 24g -type SPARSE -fs APFS -volname SB15WORK "$SPARSE" >/dev/null
fi
if ! mount | grep -q " $MNT "; then
    hdiutil attach -nobrowse -owners off -mountpoint "$MNT" "$SPARSE" >/dev/null
fi
mkdir -p "$MNT/starbite-macos15"
[ -f "$MNT/starbite-macos15/keys.txt" ] || cp "$STICK/starbite-macos15/keys.txt" "$MNT/starbite-macos15/keys.txt"
echo "work image $SPARSE at $MNT ($(df -h "$MNT" | tail -1 | awk '{print $4}') free), $(date '+%T')" | tee -a "$LOG"
# APFS has sparse files: no PREFILL; the guard watches the inner volume (if the stick goes, it goes too)
W="$MNT/starbite-macos15" PREFILL= RESERVE_MB=200 VOL="$MNT" MACOS=15 CENSUS=0 \
    tools/macos/build-root-guarded.sh desktop "$LOG"
