#!/bin/bash
# S6p undo (2026-11-16): put back what tools/macos/s6p-sandbox-icons-audio.sh changed on the card, from the copies
# that step kept in build/macos15/s6p/card.   Phone in TWRP.   tools/macos/s6p-undo.sh
#   METAL_ONLY=1  only the Metal client list (one in-place write, seconds): enough when the trouble is
#                 iconservicesagent / WindowServer; the launchd files stay as S6p left them.
# Full undo: launchd.plist and launchd-disabled.plist as before S6p (coreaudiod off, SoftwareUpdateNotificationManager
# back), the coreaudiod job plist renamed to .starbite-off again, the client list back to WindowServer + sbmtltest.
# Diagnostics v2 stays (it does nothing without sbdiag=1). Two cycles like the step itself: between them the card
# has no launchd.plist (RESUME=2 reruns cycle 2).
set -euo pipefail
cd "$(dirname "$0")/../.."
T=tools/macos/card-root-meta-sync.py
G=tools/macos/card-file-get.py
D=build/macos15/s6p
X=/System/Library/xpc
CA=/System/Library/LaunchDaemons/com.apple.audio.coreaudiod.plist
M=/private/var/local/mtl2-clients

OLD=$(shasum -a 256 tools/macos/metal/mtl2-clients | cut -d' ' -f1)
NEW=$(shasum -a 256 tools/macos/metal/mtl2-clients.icons | cut -d' ' -f1)
mkdir -p "$D/undo"
python3 $G $M "$D/undo/mtl2-clients.now"
if [ "$(shasum -a 256 "$D/undo/mtl2-clients.now" | cut -d' ' -f1)" = "$OLD" ]; then
	echo "$M is already the WindowServer-only list"
else
	python3 tools/macos/card-file-replace.py "$M" tools/macos/metal/mtl2-clients --expect-old "$NEW" --dry-run
	python3 tools/macos/card-file-replace.py "$M" tools/macos/metal/mtl2-clients --expect-old "$NEW"
fi
if [ "${METAL_ONLY:-0}" = 1 ]; then
	echo "S6P UNDO OK (Metal client list only)"
	exit 0
fi

[ -s "$D/card/launchd.plist" ] && [ -s "$D/card/launchd-disabled.plist" ] && [ -s "$D/card/com.apple.audio.coreaudiod.plist" ] \
	|| { echo "no originals in $D/card: nothing else changed"; exit 1; }
plutil -lint "$D/card/launchd.plist" "$D/card/launchd-disabled.plist"
rm -rf "$D/undo/add"
mkdir -p "$D/undo/add$X" "$D/undo/add/System/Library/LaunchDaemons"
cp "$D/card/launchd.plist" "$D/card/launchd-disabled.plist" "$D/undo/add$X/"
cp "$D/card/com.apple.audio.coreaudiod.plist" "$D/undo/add$CA.starbite-off"
cat > "$D/undo/remove.txt" <<EOF2
# S6p undo cycle 1
$X/launchd.plist
$X/launchd-disabled.plist
$CA
EOF2
if [ "${RESUME:-1}" = 1 ]; then
	I=$D/undo/root-card-rm.img
	python3 $T pull "$I"
	python3 $T remove "$I" "$D/undo/remove.txt"
	python3 $T plan "$I"
	python3 $T push "$I" --dry-run
	python3 $T push "$I"
	rm -f "$I" "$I".*
	echo "S6P UNDO cycle 1 pushed (the card has no launchd.plist now: do not boot macOS; RESUME=2 runs cycle 2 again)"
fi
I=$D/undo/root-card-add.img
python3 $T pull "$I"
python3 $T add "$I" "$D/undo/add"
python3 $T plan "$I"
python3 $T push "$I" --dry-run
python3 $T push "$I"
rm -f "$I" "$I".*
echo "S6P UNDO OK: launchd files and the Metal client list as before S6p"
