#!/bin/bash
# S3zt (2026-11-15 night): the desktop shows Sequoia Sunrise's still picture instead of plain yellow.
# S3zs: with kernel 0454 (sweetappsandbox=2) the wallpaper extension runs, but the default choice is the Sequoia
# Sunrise video (.wallpapers/Sequoia Sunrise/Sequoia Sunrise.mov, 171 MiB, left out of the card root by the size
# cap; software video decoding would cost CPU anyway). Its still frame (Sequoia Sunrise.heic, 6 MB) is on the root.
# S3zu (= this script again): the store in macOS 15.6's own "linked" layout (S3zt's macOS 27 layout was rejected).
# S3zw (= this script again): the image is listed in the choice's Files too (S3zu: empty Files gave the error wallpaper).
# Cycle 1 removes the store WallpaperAgent wrote; cycle 2 adds a store whose desktop is that image
# (tools/macos/wallpaper/mk-index-plist.py), owned by the user (501:20). Two cycles: blocks freed and reused in
# one image would not be pushed whole (s3zg).   Phone in TWRP.   Then boot a payload with sweetappsandbox=2.
set -euo pipefail
cd "$(dirname "$0")/../.."
T=tools/macos/card-root-meta-sync.py
D=build/macos15/s3zt
S="$D/src/Users/starbite/Library/Application Support/com.apple.wallpaper/Store"
mkdir -p "$S"
rm -f "$S/Index.plist"; python3 -I tools/macos/wallpaper/mk-index-plist.py "$S/Index.plist"    # always the current layout
plutil -lint "$S/Index.plist"
I=$D/root-card-rm.img
python3 $T pull "$I"
python3 $T remove "$I" tools/macos/wallpaper/s3zt-remove.txt
python3 $T plan "$I"
python3 $T push "$I" --dry-run
python3 $T push "$I"
rm -f "$I" "$I".*
I=$D/root-card-add.img
python3 $T pull "$I"
python3 $T add "$I" "$D/src" --owner 501:20
python3 $T plan "$I"
python3 $T push "$I" --dry-run
python3 $T push "$I"
rm -f "$I" "$I".*
echo "S3ZT OK: wallpaper store = Sequoia Sunrise still image"
