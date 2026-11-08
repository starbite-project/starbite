#!/usr/bin/env bash
# Make iOS 26 rebuild its LaunchServices database (the registry of apps and app extensions: posters, Control
# Center controls and widgets are extensions; on 2026-11-08 no extension process ever ran). Phone in TWRP, after
# the previous overlay was installed (one overlay at a time in the card area).
#   tools/ios26-ls-rebuild.sh          find *.csstore (read only), queue "move aside" renames (SBOVL002, kernel
#                                      0406+: boot ios26t4 / v3), write the undo list
#   tools/ios26-ls-rebuild.sh --undo DIR   queue the renames back (DIR = the build/ios26/ls-rebuild-<time> folder)
# lsd finds no database at its next start and builds a new one from the bundles on the root.
set -euo pipefail
cd "$(dirname "$0")/.."
if [ "${1:-}" = --undo ]; then
    dir=${2:?usage: ios26-ls-rebuild.sh --undo build/ios26/ls-rebuild-<time>}
    args=(); while IFS= read -r l; do [ -n "$l" ] && args+=("$l"); done < "$dir/undo.args"
    python3 tools/overlay/sboverlay.py "$dir/undo.bin" "${args[@]}" | tail -1
    tools/overlay/card-overlay.sh "$dir/undo.bin"
    echo "undo queued (${#args[@]} renames back)"; exit 0
fi
dir=build/ios26/ls-rebuild-$(date +%Y%m%d-%H%M%S); mkdir -p "$dir"
python3 tools/ios26-find.py '*.csstore' | tee "$dir/found.txt"
stamp=starbite-off-$(date +%Y%m%d%H%M)
: > "$dir/fix.args"; : > "$dir/undo.args"
while read -r kind mode owner size fpath; do
    [ "$kind" = file ] || continue
    echo "rename:$fpath=$fpath.$stamp" >> "$dir/fix.args"
    echo "rename:$fpath.$stamp=$fpath" >> "$dir/undo.args"
done < "$dir/found.txt"
n=$(grep -c . "$dir/fix.args" || true)
[ "$n" -gt 0 ] || { echo "no *.csstore found: nothing queued (see $dir/found.txt)"; exit 1; }
args=(); while IFS= read -r l; do [ -n "$l" ] && args+=("$l"); done < "$dir/fix.args"
python3 tools/overlay/sboverlay.py "$dir/fix.bin" "${args[@]}" | tail -1
tools/overlay/card-overlay.sh "$dir/fix.bin"
echo "queued: $n LaunchServices database file(s) moved aside at the next iOS 26 boot (kernel 0406+);"
echo "check 'STARLTE OVERLAY: rename ... : 0'; undo: tools/ios26-ls-rebuild.sh --undo $dir"
