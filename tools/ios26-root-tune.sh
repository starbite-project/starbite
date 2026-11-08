#!/usr/bin/env bash
# One root round for iOS 26 on the card, phone in TWRP (kernel 0401+ installs the result at the next boot):
#   1. tools/ios26-collect.py: health check, listings, /private/var/tmp + /private/var/local logs, newest
#      crash reports, launchd.plist (+ launchd-disabled.plist) -> build/test-boots/<time>-ios26collect/
#   2. tools/ios26-launchd-tune.py: jobs in tools/starlte-disabled-ios26-b.list out, PosterBoard (and any
#      LOG=... names) logging to /private/var/tmp/<name>.log
#   3. sboverlay.py + tools/overlay/card-overlay.sh: queued on the card (read back)
#   tools/ios26-root-tune.sh            collect + tune + queue
#   tools/ios26-root-tune.sh --collect  collect only (nothing written)
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "phone not in TWRP"; exit 1; }
out=build/test-boots/$(date +%Y%m%d-%H%M%S)-ios26collect
python3 tools/ios26-collect.py "$out" 2>&1 | tee "$out.log"
[ "${1:-}" = --collect ] && { echo "collected: $out"; exit 0; }
L="$out/System__Library__xpc__launchd.plist"
D="$out/System__Library__xpc__launchd-disabled.plist"
[ -s "$L" ] || { echo "launchd.plist not pulled: not tuning"; exit 1; }
[ -s "$D" ] || D=-
logs=(--log PosterBoard); for n in ${LOG:-}; do logs+=(--log "$n"); done
python3 tools/ios26-launchd-tune.py "$L" "$D" "$out/tune" --off-list tools/starlte-disabled-ios26-b.list "${logs[@]}"
python3 tools/overlay/sboverlay.py "$out/overlay.bin" \
    "$out/tune/launchd.plist=/System/Library/xpc/launchd.plist:644" \
    "$out/tune/launchd-disabled.plist=/System/Library/xpc/launchd-disabled.plist:644"
tools/overlay/card-overlay.sh "$out/overlay.bin"
echo "queued for the next iOS 26 boot (0401+ kernel): $out/tune/report.txt; undo: cp the pulled plists back the same way"
