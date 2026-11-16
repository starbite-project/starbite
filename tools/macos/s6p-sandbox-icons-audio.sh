#!/bin/bash
# S6p (2026-11-16): the card side of the S6j findings.   Phone in TWRP.   tools/macos/s6p-sandbox-icons-audio.sh
#
# 1. Audio daemon back on. tools/macos/sequoia-disabled.list took com.apple.audio.coreaudiod out (it crash-looped on
#    Ventura). Without it nobody owns the Mach service com.apple.audio.audiohald, and S6j's launchd log is full of
#    "failed lookup: name = com.apple.audio.audiohald" from heard, universalaccessd, ControlCenter, mediaremoted,
#    systemstats and every app, each asking again about 40 times a second: launchd used 15 % of a core on an idle
#    desktop, the askers 2 % each. This step shows whether coreaudiod stays up on Sequoia with no audio device.
# 2. SoftwareUpdateNotificationManager out: it asks for com.apple.softwareupdated (off on purpose) 10 times a second.
# 3. iconservicesagent gets the Mali Metal device (/private/var/local/mtl2-clients, same size, replaced in place):
#    Core Image in it says "No Metal renderer available ... No supported back-end renderer is usable", the likely
#    reason System Settings' icons are missing or wrong. Only that one process is added (S3ze's "*" cost 1.4 GB wired).
# 4. Diagnostics v2 (/Library/Starbite/sbdiag.sh; still only with sbdiag=1 on the command line).
#
# The kernel half (0466, sandbox extensions, boot-arg sweetsbext=1) is in the S6p payload, not on the card.
# Two cycles, never one (s3zg). BETWEEN THE CYCLES THE CARD HAS NO launchd.plist: macOS must not be booted until
# "S6P OK". If the script stops after "cycle 1 pushed", run it again with RESUME=2 (the card's originals stay in
# build/macos15/s6p/card). Undo: the same step with build/macos15/s6p/card's files (and mtl2-clients swapped).
set -euo pipefail
cd "$(dirname "$0")/../.."
T=tools/macos/card-root-meta-sync.py
G=tools/macos/card-file-get.py
D=build/macos15/s6p
X=/System/Library/xpc
CA=/System/Library/LaunchDaemons/com.apple.audio.coreaudiod.plist
M=/private/var/local/mtl2-clients

if [ "${RESUME:-1}" = 1 ]; then
	rm -rf "$D"
	mkdir -p "$D/card"
	python3 $G $X/launchd.plist "$D/card/launchd.plist"
	python3 $G $X/launchd-disabled.plist "$D/card/launchd-disabled.plist"
	python3 $G $CA.starbite-off "$D/card/com.apple.audio.coreaudiod.plist"
fi
[ -s "$D/card/launchd.plist" ] && [ -s "$D/card/launchd-disabled.plist" ] && [ -s "$D/card/com.apple.audio.coreaudiod.plist" ] \
	|| { echo "no card copies in $D/card"; exit 1; }
rm -rf "$D/new" "$D/add" "$D/root"
python3 -I tools/macos/launchd-enable-jobs.py "$D/card/launchd.plist" "$D/card/launchd-disabled.plist" "$D/new" \
	com.apple.audio.coreaudiod
# starlte-disable-jobs.py works on a root directory: give it one holding just the two files
mkdir -p "$D/root$X"
cp "$D/new/launchd.plist" "$D/new/launchd-disabled.plist" "$D/root$X/"
echo com.apple.SoftwareUpdateNotificationManager > "$D/disable.list"
python3 -I tools/starlte-disable-jobs.py "$D/root" "$D/disable.list"
plutil -lint "$D/root$X/launchd.plist" "$D/root$X/launchd-disabled.plist"
python3 -I - "$D/root$X/launchd.plist" <<'PY'
import plistlib, sys
d = plistlib.load(open(sys.argv[1], 'rb'))['LaunchDaemons']
labels = {j.get('Label') for j in d.values()}
assert 'com.apple.audio.coreaudiod' in labels, 'coreaudiod is not in the new launchd.plist'
assert 'com.apple.SoftwareUpdateNotificationManager' not in labels, 'SoftwareUpdateNotificationManager is still in'
assert 'com.apple.metadata.mds' in labels and 'com.apple.WindowServer' in labels, 'the new launchd.plist lost jobs'
print(f'new launchd.plist: {len(d)} jobs; coreaudiod in, SoftwareUpdateNotificationManager out')
PY

mkdir -p "$D/add$X" "$D/add/System/Library/LaunchDaemons" "$D/add/Library/Starbite"
cp "$D/root$X/launchd.plist" "$D/root$X/launchd-disabled.plist" "$D/add$X/"
cp "$D/card/com.apple.audio.coreaudiod.plist" "$D/add$CA"
cp tools/macos/diag/sbdiag.sh "$D/add/Library/Starbite/"
chmod 755 "$D/add/Library/Starbite/sbdiag.sh"
cat > "$D/remove.txt" <<EOF2
# S6p cycle 1: the files cycle 2 puts back changed
$X/launchd.plist
$X/launchd-disabled.plist
$CA.starbite-off
/Library/Starbite/sbdiag.sh
EOF2

if [ "${RESUME:-1}" = 1 ]; then
	I=$D/root-card-rm.img
	python3 $T pull "$I"
	python3 $T remove "$I" "$D/remove.txt"
	python3 $T plan "$I"
	python3 $T push "$I" --dry-run
	python3 $T push "$I"
	rm -f "$I" "$I".*
	echo "S6P cycle 1 pushed (the card has no launchd.plist now: do not boot macOS; RESUME=2 runs cycle 2 again)"
fi
I=$D/root-card-add.img
python3 $T pull "$I"
python3 $T add "$I" "$D/add"
python3 $T plan "$I"
python3 $T push "$I" --dry-run
python3 $T push "$I"
rm -f "$I" "$I".*
# the Metal client list, in place (same size); a rerun finds the new file already there
OLD=$(shasum -a 256 tools/macos/metal/mtl2-clients | cut -d' ' -f1)
NEW=$(shasum -a 256 tools/macos/metal/mtl2-clients.icons | cut -d' ' -f1)
python3 $G $M "$D/card/mtl2-clients.now"
if [ "$(shasum -a 256 "$D/card/mtl2-clients.now" | cut -d' ' -f1)" = "$NEW" ]; then
	echo "$M already lists iconservicesagent"
else
	python3 tools/macos/card-file-replace.py "$M" tools/macos/metal/mtl2-clients.icons --expect-old "$OLD" --dry-run
	python3 tools/macos/card-file-replace.py "$M" tools/macos/metal/mtl2-clients.icons --expect-old "$OLD"
fi
echo "S6P OK: coreaudiod enabled, SoftwareUpdateNotificationManager out, iconservicesagent a Metal client, diagnostics v2"
