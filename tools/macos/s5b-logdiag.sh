#!/bin/bash
# S5b (2026-11-15): read Notification Center's and the wallpaper programs' own error messages. `log show` works on
# the phone (S3zy), so a one-off login agent (tools/macos/test-agents/org.starbite.logdiag.plist) prints their
# error / container / image lines to the console as "SBLOG ..." at ~130 s after login.
# Cycle 1 removes S3zy's display diagnostic agent; cycle 2 adds this one (two cycles: blocks freed and reused in
# one image would not be pushed whole, s3zg).   Phone in TWRP.   Then boot the everyday payload.
set -euo pipefail
cd "$(dirname "$0")/../.."
T=tools/macos/card-root-meta-sync.py
D=build/macos15/s5b
rm -rf "$D/src"
mkdir -p "$D/src/Library/LaunchAgents"
cp tools/macos/test-agents/org.starbite.logdiag.plist "$D/src/Library/LaunchAgents/"
I=$D/root-card-rm.img
python3 $T pull "$I"
python3 $T remove "$I" tools/macos/test-agents/s5b-remove.txt
python3 $T plan "$I"
python3 $T push "$I" --dry-run
python3 $T push "$I"
rm -f "$I" "$I".*
I=$D/root-card-add.img
python3 $T pull "$I"
python3 $T add "$I" "$D/src"
python3 $T plan "$I"
python3 $T push "$I" --dry-run
python3 $T push "$I"
rm -f "$I" "$I".*
echo "S5B OK: dispdiag removed, logdiag added"
