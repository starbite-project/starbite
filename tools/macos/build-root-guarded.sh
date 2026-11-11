#!/usr/bin/env bash
# Run tools/macos/build-macos-root.sh with a guard for a USB work volume that drops off under heavy writes
# (2026-11-11: SBWORK vanished mid-build, newfs_hfs I/O error). Every 10 s the guard checks that the volume is
# still mounted and writable; if not, it stops the build's process group, detaches nothing (the image's attachment
# went with the volume), and says how to resume.
#   VOL=/Volumes/SBWORK MACOS=15 tools/macos/build-root-guarded.sh desktop [LOG]
set -uo pipefail
cd "$(dirname "$0")/../.."
VOL=${VOL:?VOL=/Volumes/...}
tier=${1:?tier}; log=${2:-build/macos15/root-$tier-guarded.log}
mkdir -p "$(dirname "$log")"
echo "guarded build: tier $tier on $VOL, $(date '+%F %T')" >> "$log"
set -m
tools/macos/build-macos-root.sh "$tier" >> "$log" 2>&1 &
pid=$!
while kill -0 "$pid" 2>/dev/null; do
    sleep 10
    if [ ! -d "$VOL" ] || ! touch "$VOL/.starbite-alive" 2>/dev/null; then
        echo "GUARD: $VOL vanished or is not writable at $(date '+%T'): stopping the build (pid $pid)" >> "$log"
        kill -TERM -- "-$pid" 2>/dev/null; sleep 5; kill -KILL -- "-$pid" 2>/dev/null
        echo "GUARD: when the volume is back: if the image and its OS files are complete, RESUME=1 redoes from the cache step;" >> "$log"
        echo "GUARD: otherwise delete the partial image and run the tier again." >> "$log"
        exit 3
    fi
done
wait "$pid"; rc=$?
rm -f "$VOL/.starbite-alive"
echo "EXIT $rc $(date '+%F %T')" >> "$log"
exit $rc
