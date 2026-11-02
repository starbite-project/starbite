#!/usr/bin/env bash
# Boot image for the S9 from the xnu-11417 kernel: payload (kernel + ADT + boot-args), behind the
# boot menu loader -> out/xnu-boot-NAME.img. The Windows UEFI lives in the OS store now
# (tools/store/); BUNDLES=build/boot-uc3/bundles.bin appends the old "SBUEFI!!" bundle as well.
#   [ADT=...] [KERNEL=...] [LOADER=shim/starlte/out/xnu_boot.Image] [BUNDLES=...] tools/mkimage-11417.sh NAME CMDLINE_FILE
# build/boot-uc3/ holds the loader (loader.Image), its ramdisk and the bundles appended after the
# payload; they were cut back out of out/xnu-boot-ipados18-sbcb.img (2026-10-29) when the scratch
# folder that had built them was cleaned. Keep build/boot-uc3/ (or rebuild the menu, shim/).
set -euo pipefail
cd "$(dirname "$0")/.."
U=build/boot-uc3
name=$1 cmdline=$2
python3 ref/ios-herolte/tools/mkpayload.py "${KERNEL:-out/kernel.development.starlte-11417}" "${ADT:-out/starlte-ipad7.adt}" \
	out/payload-ipados18.bin --cmdline "$(cat "$cmdline")" | grep "^size"
BUNDLES=${BUNDLES:-} python3 - <<'PY'
import os
from pathlib import Path
p = Path('out/payload-ipados18.bin').read_bytes()
p += b'\0' * (-len(p) % 4096)
b = os.environ['BUNDLES']
Path('out/payload-linux-ipados18.bin').write_bytes(p + (Path(b).read_bytes() if b else b''))
PY
python3 tools/starlte-pack-boot.py pack out/xnu-boot-uc3.img "${LOADER:-$U/loader.Image}" "out/xnu-boot-$name.img" \
	--append out/payload-linux-ipados18.bin --ramdisk "${RAMDISK:-$U/ramdisk.bin}" | tail -1
shasum -a 256 "out/xnu-boot-$name.img" | awk '{print "sha256", $1}'
