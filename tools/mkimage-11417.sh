#!/usr/bin/env bash
# Boot image for the S9 from the xnu-11417 kernel: payload (kernel + ADT + boot-args) + the boot
# menu's bundles, behind the boot menu loader -> out/xnu-boot-NAME.img
#   [ADT=out/starlte-ipad7-phone.adt] tools/mkimage-11417.sh NAME CMDLINE_FILE
# build/boot-uc3/ holds the loader (loader.Image), its ramdisk and the bundles appended after the
# payload; they were cut back out of out/xnu-boot-ipados18-sbcb.img (2026-10-29) when the scratch
# folder that had built them was cleaned. Keep build/boot-uc3/ (or rebuild the menu, shim/).
set -euo pipefail
cd "$(dirname "$0")/.."
U=build/boot-uc3
name=$1 cmdline=$2
python3 ref/ios-herolte/tools/mkpayload.py out/kernel.development.starlte-11417 "${ADT:-out/starlte-ipad7.adt}" \
	out/payload-ipados18.bin --cmdline "$(cat "$cmdline")" | grep "^size"
python3 - <<'PY'
from pathlib import Path
p = Path('out/payload-ipados18.bin').read_bytes()
p += b'\0' * (-len(p) % 4096)
Path('out/payload-linux-ipados18.bin').write_bytes(p + Path('build/boot-uc3/bundles.bin').read_bytes())
PY
python3 tools/starlte-pack-boot.py pack out/xnu-boot-uc3.img $U/loader.Image "out/xnu-boot-$name.img" \
	--append out/payload-linux-ipados18.bin --ramdisk $U/ramdisk.bin | tail -1
shasum -a 256 "out/xnu-boot-$name.img" | awk '{print "sha256", $1}'
