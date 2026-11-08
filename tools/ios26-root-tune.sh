#!/usr/bin/env bash
# One root round for iOS 26 on the card, phone in TWRP (kernel 0401+ installs the result at the next boot):
#   1. tools/ios26-collect.py: health check, listings, /private/var/tmp + /private/var/local logs, newest
#      crash reports, launchd.plist (+ launchd-disabled.plist) -> build/test-boots/<time>-ios26collect/
#   2. tools/ios26-launchd-tune.py: jobs in tools/starlte-disabled-ios26-b.list out, PosterBoard (and any
#      LOG=... names) logging to /private/var/tmp/<name>.log
#   3. tools/ios26-prefs-tune.py: mobile's com.apple.Accessibility.plist with Reduce Motion and Reduce
#      Transparency on (no Liquid Glass blur or motion on the Mali path; owner 501:501 needs kernel 0403:
#      boot T3 or v2 next, not T2)
#   4. sboverlay.py + tools/overlay/card-overlay.sh: queued on the card (read back)
#   tools/ios26-root-tune.sh            collect + tune + queue
#   tools/ios26-root-tune.sh --collect  collect only (nothing written)
#   TUNE=prefs ...                      only the Accessibility prefs (launchd.plist untouched)
#   TUNE=launchd-log ...                prefs + launchd.plist with only the PosterBoard log (no job removed);
#                                       LOG="pkd lsd ..." adds those jobs' logs (LOGDEST=console: into the kernel log)
#   TUNE=full ... (default)             prefs + PosterBoard log + jobs in starlte-disabled-ios26-b.list out
#   (T3 2026-11-08 17:06: TUNE=full's launchd.plist ended in "No service cache": bisect with these)
#   tools/ios26-root-tune.sh --undo DIR re-queue the launchd.plist (+ launchd-disabled.plist if the root had
#                                       one) pulled into DIR (a <time>-ios26collect folder) before the tune:
#                                       installed at the next iOS 26 boot, as the tune was
# The pulled originals stay in the collect folder (System__Library__xpc__launchd*.plist,
# private__var__mobile__Library__Preferences__com.apple.Accessibility.plist when the root had one), sha256 in
# originals.sha256. Undo of the Accessibility plist when there was none: an empty plist (iOS defaults).
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "phone not in TWRP"; exit 1; }
if [ "${1:-}" = --undo ]; then
    dir=${2:?usage: ios26-root-tune.sh --undo build/test-boots/<time>-ios26collect}
    L="$dir/System__Library__xpc__launchd.plist" D="$dir/System__Library__xpc__launchd-disabled.plist"
    [ -s "$L" ] || { echo "$L missing"; exit 1; }
    (cd "$dir" && shasum -a 256 -c originals.sha256)
    args=("$L=/System/Library/xpc/launchd.plist:644")
    # launchd-disabled.plist is our own keep-file (launchd never reads it): only put back a copy that parses
    if [ -s "$D" ] && python3 -c 'import plistlib,sys; plistlib.load(open(sys.argv[1],"rb"))' "$D" 2>/dev/null; then
        args+=("$D=/System/Library/xpc/launchd-disabled.plist:644")
    fi
    A="$dir/private__var__mobile__Library__Preferences__com.apple.Accessibility.plist"
    [ -s "$A" ] || { A="$dir/ax-empty.plist"; python3 -c 'import plistlib,sys; plistlib.dump({}, open(sys.argv[1], "wb"), fmt=plistlib.FMT_BINARY)' "$A"; }
    args+=("$A=/private/var/mobile/Library/Preferences/com.apple.Accessibility.plist:600:501:501")
    python3 tools/overlay/sboverlay.py "$dir/undo.bin" "${args[@]}"
    tools/overlay/card-overlay.sh "$dir/undo.bin"
    echo "undo queued: the original launchd.plist goes back at the next iOS 26 boot (0401+ kernel)"
    exit 0
fi
out=build/test-boots/$(date +%Y%m%d-%H%M%S)-ios26collect
python3 tools/ios26-collect.py "$out" 2>&1 | tee "$out.log"
[ "${1:-}" = --collect ] && { echo "collected: $out"; exit 0; }
L="$out/System__Library__xpc__launchd.plist"
D="$out/System__Library__xpc__launchd-disabled.plist"
[ -s "$L" ] || { echo "launchd.plist not pulled: not tuning"; exit 1; }
(cd "$out" && shasum -a 256 System__Library__xpc__launchd*.plist private__var__mobile__Library__Preferences__com.apple.Accessibility.plist 2>/dev/null > originals.sha256 || true; cat originals.sha256)
A="$out/private__var__mobile__Library__Preferences__com.apple.Accessibility.plist"
[ -s "$A" ] || A=-
[ -s "$D" ] || D=-
mode=${TUNE:-full}
mkdir -p "$out/tune"
# every pulled plist the chosen mode reads must parse (a stale host read gives garbage: refuse, no traceback)
plist_ok() { [ "$1" = - ] || python3 -c 'import plistlib,sys; plistlib.load(open(sys.argv[1],"rb"))' "$1" 2>/dev/null; }
need=("$A"); [ "$mode" = full ] && need+=("$L" "$D"); [ "$mode" = launchd-log ] && need+=("$L")
for f in "${need[@]}"; do
    plist_ok "$f" || { echo "$f does not parse (stale host read?): refusing; nothing queued"; exit 1; }
done
# The host reads raw blocks without replaying the journal, so a recently rewritten launchd.plist can read back
# stale (2026-11-08). Only edit the known-good original (sha below, pulled 17:06 and restored by --undo).
GOOD_LAUNCHD=5d8bc695
if [ "$mode" != prefs ]; then
    got=$(shasum -a 256 "$L" | cut -c1-8)
    # known good: Apple's original as edited before today (5d8bc695) or a launchd.plist one of our tunes wrote
    good=$( { echo $GOOD_LAUNCHD; for t in build/test-boots/*-ios26collect/tune/launchd.plist; do
        [ -f "$t" ] && shasum -a 256 "$t" | cut -c1-8; done; } | sort -u)
    echo "$good" | grep -qx "$got" || { echo "pulled launchd.plist is $got, not a known-good one (stale read?): refusing to edit it"; exit 1; }
fi
files=()
case "$mode" in
prefs) ;;
launchd-log)
    # no job removed: launchd-disabled.plist is neither read nor written (it can read back stale)
    logs=(--log PosterBoard); for n in ${LOG:-}; do logs+=(--log "$n"); done
    python3 tools/ios26-launchd-tune.py "$L" - "$out/tune" "${logs[@]}"
    files+=("$out/tune/launchd.plist=/System/Library/xpc/launchd.plist:644") ;;
full)
    logs=(--log PosterBoard); for n in ${LOG:-}; do logs+=(--log "$n"); done
    python3 tools/ios26-launchd-tune.py "$L" "$D" "$out/tune" --off-list tools/starlte-disabled-ios26-b.list "${logs[@]}"
    files+=("$out/tune/launchd.plist=/System/Library/xpc/launchd.plist:644"
            "$out/tune/launchd-disabled.plist=/System/Library/xpc/launchd-disabled.plist:644") ;;
*) echo "TUNE=$mode: prefs | launchd-log | full"; exit 1 ;;
esac
python3 tools/ios26-prefs-tune.py "$A" "$out/tune/com.apple.Accessibility.plist" \
    ReduceMotionEnabled=1 EnhancedBackgroundContrastEnabled=1 | tee -a "$out/tune/report.txt"
files+=("$out/tune/com.apple.Accessibility.plist=/private/var/mobile/Library/Preferences/com.apple.Accessibility.plist:600:501:501")
echo "TUNE=$mode" >> "$out/tune/report.txt"
python3 tools/overlay/sboverlay.py "$out/overlay.bin" "${files[@]}"
tools/overlay/card-overlay.sh "$out/overlay.bin"
echo "queued for the next iOS 26 boot (0401+ kernel): $out/tune/report.txt; undo: tools/ios26-root-tune.sh --undo $out"
