#!/bin/bash
# S3za (2026-11-14): take airportd's Mach services out of the card root's launchd cache, so CoreWiFi clients
# (Control Center's Wi-Fi menu-bar item) get "no such service" at once instead of waiting forever on a daemon
# that has no Wi-Fi hardware to answer for. S3z9: the waiting Wi-Fi item animates in the menu bar every frame,
# so WindowServer presents ~17 frames a second on the idle desktop and the kernel rescales them.
#
# Phone in TWRP. Reads the card's own /System/Library/xpc/launchd.plist (read-only), renames the service names of
# com.apple.airportd in a copy (same size, every other value checked unchanged), then replaces the card file in place
# (dry run first; --expect-old = the bytes just read). Undo: the same with the saved .card copy as the new file.
#   tools/macos/s3za-mute-airportd.sh [OUTDIR]
set -euo pipefail
cd "$(dirname "$0")/../.."
D=${1:-build/macos15/s3za}
mkdir -p "$D"
F=/System/Library/xpc/launchd.plist
python3 tools/macos/card-file-get.py "$F" "$D/launchd.plist.card"
OLD=$(shasum -a 256 "$D/launchd.plist.card" | cut -d' ' -f1)
python3 tools/macos/launchd-cache-mute.py "$D/launchd.plist.card" "$D/launchd.plist.new" --label com.apple.airportd
python3 tools/macos/card-file-replace.py "$F" "$D/launchd.plist.new" --expect-old "$OLD" --dry-run
python3 tools/macos/card-file-replace.py "$F" "$D/launchd.plist.new" --expect-old "$OLD"
echo "S3ZA OK: airportd's services muted in the card's launchd cache (undo: $D/launchd.plist.card, old sha $OLD)"
