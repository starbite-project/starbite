#!/bin/bash
# S5a (2026-11-15): the 2x (Retina) default, second try. S3zy's log: WindowServer names the built-in panel by
# vendor 0x610 + the machine's "Gestalt Model ID", so the override files must sit under DisplayVendorID-610
# (tools/macos/display/mk-override.py: one per candidate product ID). One pull/add/push cycle, new files only.
# The dispdiag agent from S3zy stays for this run (its log shows the mode list again); remove it afterwards.
# Phone in TWRP.   Then boot payload xnu-macos15-s5a.
set -euo pipefail
cd "$(dirname "$0")/../.."
T=tools/macos/card-root-meta-sync.py
D=build/macos15/s5a
rm -rf "$D/src"
mkdir -p "$D/src"
python3 -I tools/macos/display/mk-override.py "$D/src"
I=$D/root-card.img
python3 $T pull "$I"
python3 $T add "$I" "$D/src"
python3 $T plan "$I"
python3 $T push "$I" --dry-run
python3 $T push "$I"
rm -f "$I" "$I".*
echo "S5A OK: display overrides under DisplayVendorID-610 added"
