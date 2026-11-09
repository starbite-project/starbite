#!/usr/bin/env bash
# iOS 26: shrink metal2vulkan's pooled GPU memory blocks in the deployed StarbiteMetal (backboardd held 17 x 24 MiB,
# 408 MiB, on 2026-11-09) by patching one immediate (tools/metal2/patch-block-bytes.py); phone in TWRP; kernel 0409+.
#   tools/ios26-metal-blocks.sh [MIB]     default 8: find the bundle, pull its executable (read only), patch, queue
#   tools/ios26-metal-blocks.sh --undo    queue the pulled original back
# Files: build/ios26/sm/{path.txt,StarbiteMetal.orig,StarbiteMetal.patched,ov.bin,undo.bin}
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "phone not in TWRP"; exit 1; }
D=build/ios26/sm; mkdir -p "$D"
if [ "${1:-}" = --undo ]; then
    [ -s "$D/StarbiteMetal.orig" ] && [ -s "$D/path.txt" ] || { echo "no pulled original in $D"; exit 1; }
    python3 tools/overlay/sboverlay.py "$D/undo.bin" "$D/StarbiteMetal.orig=$(cat "$D/path.txt"):755" | tail -1
    tools/overlay/card-overlay.sh "$D/undo.bin"; echo "undo queued"; exit 0
fi
mib=${1:-8}
if [ ! -s "$D/StarbiteMetal.orig" ]; then
    found=$(python3 tools/ios26-find.py 'StarbiteMetal' /System/Library /usr --depth 5 | awk '$1=="file"{print $NF}')
    [ "$(echo "$found" | grep -c .)" = 1 ] || { echo "StarbiteMetal executable not found exactly once: $found"; exit 1; }
    echo "$found" > "$D/path.txt"
    HFS_CARD=1 python3 tools/overlay/hfs-read.py "$found" "$D/StarbiteMetal.orig"
fi
python3 tools/metal2/patch-block-bytes.py "$D/StarbiteMetal.orig" "$D/StarbiteMetal.patched" "$mib"
python3 tools/overlay/sboverlay.py "$D/ov.bin" "$D/StarbiteMetal.patched=$(cat "$D/path.txt"):755" | tail -1
tools/overlay/card-overlay.sh "$D/ov.bin"
echo "queued: $(cat "$D/path.txt") with ${mib} MiB blocks (needs kernel 0409+); undo: tools/ios26-metal-blocks.sh --undo"
