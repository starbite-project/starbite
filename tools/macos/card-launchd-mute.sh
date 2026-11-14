#!/bin/bash
# Rename the Mach services of the given launchd jobs in the card root's launchd cache (/System/Library/xpc/launchd.plist),
# so clients' lookups fail at once instead of launching a job that cannot work here (configuration only; the
# generalised S3za step, tools/macos/s3za-mute-airportd.sh).
#
# Phone in TWRP. Reads the card's own copy (read-only), renames the service names of the labels in a copy (same size,
# every other value checked unchanged), dry run, then replaces the card file in place with --expect-old = the bytes
# just read. Undo: the same with the saved .card copy as the new file.
#   tools/macos/card-launchd-mute.sh OUTDIR LABEL [LABEL ...]
set -euo pipefail
cd "$(dirname "$0")/../.."
D=$1; shift
[ $# -ge 1 ] || { echo "usage: $0 OUTDIR LABEL [LABEL ...]" >&2; exit 2; }
mkdir -p "$D"
F=/System/Library/xpc/launchd.plist
python3 tools/macos/card-file-get.py "$F" "$D/launchd.plist.card"
OLD=$(shasum -a 256 "$D/launchd.plist.card" | cut -d' ' -f1)
args=()
for l in "$@"; do args+=(--label "$l"); done
python3 tools/macos/launchd-cache-mute.py "$D/launchd.plist.card" "$D/launchd.plist.new" "${args[@]}"
python3 tools/macos/card-file-replace.py "$F" "$D/launchd.plist.new" --expect-old "$OLD" --dry-run
python3 tools/macos/card-file-replace.py "$F" "$D/launchd.plist.new" --expect-old "$OLD"
echo "MUTE OK: $* muted in the card's launchd cache (undo: $D/launchd.plist.card, old sha $OLD)"
