#!/bin/bash
# S3zx (2026-11-15 morning): remove S3zi's unattended app-launch test agent from the card root, so apps no longer
# open by themselves ~2-5 min after login (Settings, App Store, TextEdit were tested in S3zi-S3zw).
# One pull/remove/push cycle (only metadata changes).   Phone in TWRP.   Then boot as usual (row mac15 = s3zs).
set -euo pipefail
cd "$(dirname "$0")/../.."
T=tools/macos/card-root-meta-sync.py
I=build/macos15/s3zx/root-card-rm.img
mkdir -p build/macos15/s3zx
python3 $T pull "$I"
python3 $T remove "$I" tools/macos/test-agents/s3zx-remove.txt
python3 $T plan "$I"
python3 $T push "$I" --dry-run
python3 $T push "$I"
rm -f "$I" "$I".*
echo "S3ZX OK: org.starbite.applaunch removed"
