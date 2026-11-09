#!/usr/bin/env bash
# iOS 26: os_log debug profiles for chosen subsystems, through the card overlay (phone in TWRP; kernel 0409+).
#   tools/ios26-log-profile.sh com.apple.PosterBoard com.apple.PosterKit ...   queue debug profiles
#   tools/ios26-log-profile.sh --undo com.apple.PosterBoard ...                queue empty profiles (= defaults)
# Writes /System/Library/Preferences/Logging/Subsystems/<subsystem>.plist (the folder exists; the files did not
# for the Poster* subsystems): Level Enable/Persist Debug + Enable-Private-Data. 2026-11-09 T20:
# OS_ACTIVITY_MODE=debug alone added no PosterBoard message; the mirror (OS_ACTIVITY_DT_MODE + stderr to
# /dev/console, tools/ios26-root-tune.sh with LOGDEST=console) prints what the profile enables.
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "phone not in TWRP"; exit 1; }
undo=0
[ "${1:-}" = --undo ] && { undo=1; shift; }
[ $# -gt 0 ] || { sed -n '2,8p' "$0"; exit 1; }
D=build/ios26/logprof
mkdir -p "$D"
args=()
for s in "$@"; do
    case "$s" in *[!A-Za-z0-9._-]*|'') echo "bad subsystem name: $s"; exit 1;; esac
    python3 - "$D/$s.plist" "$undo" <<'PY'
import plistlib, sys
body = {} if sys.argv[2] == '1' else {
    'DEFAULT-OPTIONS': {'Level': {'Enable': 'Debug', 'Persist': 'Debug'}, 'Enable-Private-Data': True}}
plistlib.dump(body, open(sys.argv[1], 'wb'))
PY
    args+=("$D/$s.plist=/System/Library/Preferences/Logging/Subsystems/$s.plist:644:0:0")
done
python3 tools/overlay/sboverlay.py "$D/ov.bin" "${args[@]}" | tail -1
tools/overlay/card-overlay.sh "$D/ov.bin"
echo "queued: $([ $undo = 1 ] && echo empty || echo debug) profiles for $* (installed at the next boot)"
