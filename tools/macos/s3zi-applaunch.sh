#!/bin/bash
# S3zi (2026-11-14 night): an unattended app test. Adds /Library/LaunchAgents/org.starbite.applaunch.plist
# (tools/macos/test-agents): at login it opens System Settings at ~140 s, App Store at ~210 s and TextEdit at ~280 s,
# each announced as "SBLAUNCH ..." on the console; the S3zi payload dumps frames at 180/240/300 s. Remove the agent
# again (card-root-meta-sync remove) once apps are tested by hand.   Phone in TWRP.
set -euo pipefail
cd "$(dirname "$0")/../.."
I=build/macos15/s3zi/root-card.img
T=tools/macos/card-root-meta-sync.py
python3 $T pull "$I"
python3 $T add "$I" build/macos15/s3zi/src
python3 $T plan "$I"
python3 $T push "$I" --dry-run
python3 $T push "$I"
rm -f "$I" "$I".*
echo "S3ZI OK: org.starbite.applaunch added"
