#!/bin/bash
# S3zy (2026-11-15): the 2x (Retina) mode. Adds to the card root (one pull/add/push cycle, new files only):
#  - the S9 panel's display override files (tools/macos/display/mk-override.py): target-default-ppmm makes
#    WindowServer pick the 2x mode as the default (720 x 1480 points on the native 1440 x 2960 canvas);
#  - a one-off diagnostic login agent (tools/macos/test-agents/org.starbite.dispdiag.plist): prints
#    system_profiler's display report and WindowServer's "Display:" log lines (the mode list) to the console,
#    then opens System Settings. Remove it after the test (card-root-meta-sync remove).
# Phone in TWRP.   Then boot payload xnu-macos15-s3zy (kernel seq2a = 0455: the panel publishes its identity).
set -euo pipefail
cd "$(dirname "$0")/../.."
T=tools/macos/card-root-meta-sync.py
D=build/macos15/s3zy
rm -rf "$D/src"
mkdir -p "$D/src/Library/LaunchAgents"
python3 -I tools/macos/display/mk-override.py "$D/src"
cp tools/macos/test-agents/org.starbite.dispdiag.plist "$D/src/Library/LaunchAgents/"
I=$D/root-card.img
python3 $T pull "$I"
python3 $T add "$I" "$D/src"
python3 $T plan "$I"
python3 $T push "$I" --dry-run
python3 $T push "$I"
rm -f "$I" "$I".*
echo "S3ZY OK: display overrides + dispdiag agent added"
