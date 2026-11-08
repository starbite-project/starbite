#!/usr/bin/env bash
# Queue the fix for every damaged container folder/file the scan found (SBOVL002 owner/mode operations, kernel
# 0406+: T4 / v3; older kernels ignore them). Phone in TWRP.
#   tools/ios26-perm-fix.sh [SCAN]           default build/ios26/perm-scan.txt (from tools/ios26-perm-scan.py)
#   tools/ios26-perm-fix.sh --undo [SCAN]    queue the values the scan read
set -euo pipefail
cd "$(dirname "$0")/.."
undo=0; [ "${1:-}" = --undo ] && { undo=1; shift; }
scan=${1:-build/ios26/perm-scan.txt}
out=build/ios26/perm-fix
python3 tools/ios26-perm-fix.py "$scan" "$out"
f=$out/fix.args; [ $undo = 1 ] && f=$out/undo.args
args=(); while IFS= read -r l; do [ -n "$l" ] && args+=("$l"); done < "$f"
python3 tools/overlay/sboverlay.py "$out/$( [ $undo = 1 ] && echo undo || echo fix ).bin" "${args[@]}" | tail -1
tools/overlay/card-overlay.sh "$out/$( [ $undo = 1 ] && echo undo || echo fix ).bin"
echo "queued ($( [ $undo = 1 ] && echo undo || echo fix ), ${#args[@]} operations): boot ios26t4 (or v3); check the kernel log for 'STARLTE OVERLAY: setattr' lines ending in ': 0'"
