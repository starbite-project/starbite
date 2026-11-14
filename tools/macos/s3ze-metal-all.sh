#!/bin/bash
# S3ze (2026-11-14): every process gets the Mali Metal device. The card's /private/var/local/mtl2-clients listed only
# WindowServer; StarbiteMetal returned a nil device to any other process and Metal's discovery then threw
# (+[NSDictionary dictionaryWithObjects:forKeys:count:] with a nil value): Finder at its first menu (the menu bar
# vanished until Finder relaunched), System Settings at launch. tools/macos/metal/mtl2-clients.all is the same size
# as tools/macos/metal/mtl2-clients ("*"), so card-file-replace writes it in place (it refuses unless the card file is
# exactly mtl2-clients). Undo: the same with the two files swapped.  Phone in TWRP.
set -euo pipefail
cd "$(dirname "$0")/../.."
OLD=$(shasum -a 256 tools/macos/metal/mtl2-clients | cut -d' ' -f1)
F=/private/var/local/mtl2-clients
python3 tools/macos/card-file-replace.py "$F" tools/macos/metal/mtl2-clients.all --expect-old "$OLD" --dry-run
python3 tools/macos/card-file-replace.py "$F" tools/macos/metal/mtl2-clients.all --expect-old "$OLD"
echo "S3ZE OK: $F now lists every process (\"*\")"
