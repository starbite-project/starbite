#!/usr/bin/env bash
# iOS 26 kernel as an OS store entry (no BOOT image): the 12377 kernel (strip -x
# copy, as tools/mkimage-ios26.sh) + the iPhone 11 ADT + CMDLINE_FILE, packed by
# mkpayload.py into a SWEETXNU payload for tools/store/sbstore2.py (type xnu).
#   [ADT=...] [KERNEL=...] tools/mkpayload-ios26.sh NAME CMDLINE_FILE   -> out/xnu-NAME.xnu
set -euo pipefail
cd "$(dirname "$0")/.."
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode_16.4.app/Contents/Developer}
name=$1 cmdline=$2
KERNEL=${KERNEL:-out/kernel.development.starlte-12377}
ADT=${ADT:-out/starlte-iphone11.adt}
[ -f "$KERNEL" ] || { echo "no $KERNEL: tools/build-xnu-12377.sh"; exit 1; }
cp "$KERNEL" "$KERNEL.x"
xcrun strip -x "$KERNEL.x"
python3 ref/ios-herolte/tools/mkpayload.py "$KERNEL.x" "$ADT" "out/xnu-$name.xnu" \
    --cmdline "$(cat "$cmdline")" | grep -E "^(size|fixups|entry)" || true
shasum -a 256 "out/xnu-$name.xnu"
