#!/bin/bash
# S3zg (2026-11-14): (1) add SFSymbols' CoreGlyphsPrivate.bundle Assets.car (26 MB, skipped from the Sequoia root by the
# desktop build's size cap): without it AppKit's NSSearchFieldCell symbol-image dictionary gets a nil image and throws
# (S3zf's 40-frame backtraces: Finder at its first menu/window, System Settings at launch); (2) put the original
# mtl2-clients back (WindowServer only): S3ze's "*" fixed nothing and every app's GPU buffers pushed wired memory to
# 1.4 GB until launchd's code could not be paged back in (S3zf panic at 423 s).
# Phone in TWRP.   tools/macos/s3zg-glyphs-and-metal-list.sh [IMG]
set -euo pipefail
cd "$(dirname "$0")/../.."
I=${1:-build/macos15/s3zg/root-card.img}
T=tools/macos/card-root-meta-sync.py
python3 $T pull "$I"
python3 $T add "$I" build/macos15/s3zg/src
python3 $T plan "$I"
python3 $T push "$I" --dry-run
python3 $T push "$I"
NEW=$(shasum -a 256 tools/macos/metal/mtl2-clients.all | cut -d' ' -f1)
F=/private/var/local/mtl2-clients
python3 tools/macos/card-file-replace.py "$F" tools/macos/metal/mtl2-clients --expect-old "$NEW" --dry-run
python3 tools/macos/card-file-replace.py "$F" tools/macos/metal/mtl2-clients --expect-old "$NEW"
echo "S3ZG OK: CoreGlyphsPrivate Assets.car added; $F back to WindowServer only"
