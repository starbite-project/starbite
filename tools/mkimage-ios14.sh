#!/usr/bin/env bash
# iOS 14 XNU payload and boot image: the xnu-7195 STARLTE kernel (strip -x
# copy) and the iPhone 7 ADT (tools/mkadt_starlte.py --iphone7, from the real
# D10AP device tree of iOS 14.8).
#   tools/mkimage-ios14.sh NAME CMDLINE_FILE
#       -> out/payload-NAME.bin        (SWEETXNU payload = the OS store entry "ios14.xnu")
#       -> out/xnu-boot-NAME.img       (BOOT image carrying it as "@boot", for a one-off test)
#   MDROOT=FILE   append FILE as the md0 RAM disk (boot with rd=md0); with the kernel's mockfs a
#                 single executable is the whole root, e.g. the bring-up init:
#                 MDROOT=build/ios14/sweet-init tools/mkimage-ios14.sh ios14-k1 tools/cmdline/ios14-kernel-test.txt
#   ADT=... KERNEL=... LOADER=... override the inputs; the ADT is built when missing.
set -euo pipefail
cd "$(dirname "$0")/.."
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode_15.4.app/Contents/Developer}
name=$1 cmdline=$2
U=build/boot-uc3
KERNEL=${KERNEL:-out/kernel.development.starlte-7195}
ADT=${ADT:-out/starlte-iphone7.adt}
[ -f "$KERNEL" ] || { echo "no $KERNEL: tools/build-xnu-7195.sh"; exit 1; }
if [ ! -f "$ADT" ]; then
    if [ ! -f build/ipsw-14/d10ap.adt ]; then
        DEVICE=iPhone9,1 VERSION=14.8 BOARD=d10ap OUT=build/ipsw-14 PARTS=devicetree tools/fetch-ios-parts.sh
        build/venv/bin/pyimg4 im4p extract -i build/ipsw-14/devicetree.im4p -o build/ipsw-14/d10ap.adt
    fi
    python3 tools/mkadt_starlte.py "$ADT" --iphone7 build/ipsw-14/d10ap.adt --big
fi
cp "$KERNEL" "$KERNEL.x"
xcrun strip -x "$KERNEL.x"
payload=out/payload-$name.bin
python3 ref/ios-herolte/tools/mkpayload.py "$KERNEL.x" "$ADT" "$payload" \
    --cmdline "$(cat "$cmdline")" ${MDROOT:+--ramdisk "$MDROOT"} | grep -E "^(size|fixups|ramdisk|entry)"
shasum -a 256 "$payload"
# BOOT image: loader + its ramdisk, the payload appended (as tools/mkimage-11417.sh).
python3 - "$payload" "out/payload-linux-ios14-$name.bin" <<'PY'
import sys
from pathlib import Path
p = Path(sys.argv[1]).read_bytes()
Path(sys.argv[2]).write_bytes(p + b'\0' * (-len(p) % 4096))
PY
python3 tools/starlte-pack-boot.py pack out/xnu-boot-uc3.img "${LOADER:-$U/loader.Image}" "out/xnu-boot-$name.img" \
    --append "out/payload-linux-ios14-$name.bin" --ramdisk "${RAMDISK:-$U/ramdisk.bin}" | tail -1
rm -f "out/payload-linux-ios14-$name.bin"
shasum -a 256 "out/xnu-boot-$name.img"
