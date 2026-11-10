#!/usr/bin/env bash
# iOS 26: add entitlements to one binary on the card root (phone in TWRP; kernel 0409+ for the overlay).
#   tools/ios26-entitle.sh /Applications/PosterBoard.app/PosterBoard com.apple.private.extensionkit.host.any-extension
#   tools/ios26-entitle.sh --undo /Applications/PosterBoard.app/PosterBoard
# Pulls the binary (kept as build/ios26/ent/<name>.orig for --undo), adds each KEY = true to its entitlements,
# re-signs it ad hoc with XML + DER entitlement blobs (every signed binary on the root is a platform binary to
# our AMFI, bsd/kern/sweet_amfi.c, and enforcement is off), checks the DER blob, and queues it with the original
# owner and mode. 2026-11-10: ExtensionKit refused PosterBoard's poster query in its host check
# (ExtensionFoundation: host.any-extension or extension-management passes it outright).
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "phone not in TWRP"; exit 1; }
undo=0
[ "${1:-}" = --undo ] && { undo=1; shift; }
target=${1:?PATH}; shift
name=$(basename "$target")
D=build/ios26/ent; mkdir -p "$D"
# owner and mode from the catalog
line=$(python3 tools/ios26-find.py "$name" "$(dirname "$target")" --depth 1 | awk -v t="$target" '$1=="file" && $NF==t')
[ -n "$line" ] || { echo "$target not found on the card"; exit 1; }
mode=$(echo "$line" | awk '{print $2}'); owner=$(echo "$line" | awk '{print $3}')
if [ ! -s "$D/$name.orig" ]; then
    HFS_CARD=1 python3 tools/overlay/hfs-read.py "$target" "$D/$name.orig" >/dev/null
fi
if [ $undo = 1 ]; then
    src="$D/$name.orig"
else
    [ $# -gt 0 ] || { echo "no entitlement keys"; exit 1; }
    cp "$D/$name.orig" "$D/$name.patched"
    codesign -d --entitlements :- "$D/$name.orig" 2>/dev/null > "$D/$name.ent.plist" || true
    # keys contain dots, which plutil reads as key paths: edit with plistlib
    python3 - "$D/$name.ent.plist" "$@" <<'PY'
import plistlib, sys
p = sys.argv[1]
try:
    d = plistlib.load(open(p, 'rb'))
except Exception:
    d = {}
for k in sys.argv[2:]:
    d[k] = True
plistlib.dump(d, open(p, 'wb'))
PY
    ident=$(codesign -dv "$D/$name.orig" 2>&1 | sed -n 's/^Identifier=//p')
    codesign -f -s - -i "$ident" --entitlements "$D/$name.ent.plist" --generate-entitlement-der "$D/$name.patched" 2>&1 | grep -v "replacing existing signature" || true
    for k in "$@"; do
        python3 - "$D/$name.patched" "$k" <<'PY'
import sys
b = open(sys.argv[1], 'rb').read()
i = b.find(b'\xfa\xde\x71\x72')
ok = i >= 0 and sys.argv[2].encode() in b[i:i + int.from_bytes(b[i + 4:i + 8], 'big')]
sys.exit(0 if ok else f'{sys.argv[2]}: not in the DER entitlements')
PY
    done
    src="$D/$name.patched"
fi
uid=${owner%%:*}; gid=${owner##*:}
python3 tools/overlay/sboverlay.py "$D/$name.ov.bin" "$src=$target:$mode:$uid:$gid" | tail -1
tools/overlay/card-overlay.sh "$D/$name.ov.bin"
echo "queued: $target ($( [ $undo = 1 ] && echo original || echo "+ $*" ), mode $mode owner $owner)"
