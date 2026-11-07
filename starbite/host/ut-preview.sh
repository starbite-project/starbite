#!/bin/sh
# Utility OS host preview (docs/UTILITY-OS.md), like starbite/host/preview.sh for Lab: the real Utility OS
# and sbui code built for aarch64 Linux (the Lima VM) on host.c's fake hardware, driven by scripted keys
# and taps, PNGs to starbite/utility/preview/.  Run on the Mac from the repo root:
#   sh starbite/host/ut-preview.sh          every scene
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
out="$root/starbite/out/uthost"
png="$root/starbite/utility/preview"
py="$root/build/venv/bin/python"
[ -x "$py" ] || py="$root/../../../build/venv/bin/python"
mkdir -p "$out" "$png"
lib="$root/starbite/libsb"
ut="$root/starbite/utility"
SB7=""
for c in "$root/build/brand/menu7.sb7" "$root/../../../build/brand/menu7.sb7"; do
    if [ -f "$c" ]; then SB7=$c; break; fi
done
[ -n "$SB7" ] || { echo "no build/brand/menu7.sb7"; exit 1; }
limactl shell --workdir "$root" starbite python3 starbite/tools/mkassets.py "$SB7" "$out/sbassets.sb7" >/dev/null
limactl shell --workdir "$root" starbite gcc -O1 -g -DSB_HOST -DSB_HOST_UTILITY "-DSB_ASSETS=\"$out/sbassets.sb7\"" \
    -Wall -Wextra -Werror -Wno-unused-parameter -fno-strict-aliasing -I"$lib" -I"$ut" -I"$root/starbite/engineer" \
    -o "$out/ut-host" "$root/starbite/host/host.c" "$root/starbite/host/ut-host-stubs.c" \
    "$lib/str.c" "$lib/log.c" "$lib/sbui.c" "$lib/sbui_input.c" "$lib/sbui_page.c" "$lib/sbui_w.c" "$lib/sb7.c" \
    "$lib/fdt.c" "$lib/catalog.c" "$lib/fence.c" "$lib/tmu.c" "$lib/adc.c" "$lib/mali.c" "$lib/mali_desc.c" \
    "$lib/assets.S" "$ut/main.c" "$ut/msc.c" "$ut/fat.c" "$ut/tools.c" "$ut/pad.c" "$ut/hid.c" "$ut/hash.c" "$ut/sha256.c" "$ut/calc.c" "$ut/calcmath.c" "$ut/exfat.c" "$ut/macros.c" -lm -lpthread

limactl shell --workdir "$root" starbite sh -c "gzip -dc starbite/tools/fixtures/exfat-card.img.gz > $out/exfat-card.img"
scene() {
    name=$1
    env=$2
    shift 2
    limactl shell --workdir "$root" starbite env $env UT_HOST_CARD="$out/exfat-card.img" "$out/ut-host" "$out" /dev/null "$@" "s:$name" q | grep -E "panel|FAIL|host:" || true
    "$py" -c "from PIL import Image; Image.open('$out/$name.ppm').save('$png/$name.png')"
}
scene home        UT_HOST_USB=0 w:500
scene home-usb    UT_HOST_USB=1 w:1500
scene usb         UT_HOST_USB=1 w:500 k:p w:800
scene radio       UT_HOST_USB=1 w:500 k:d w:200 k:p w:800
scene leave       UT_HOST_USB=1 w:500 k:u w:200 k:p w:800
scene measure     UT_HOST_USB=1 w:500 k:d w:200 k:d w:200 k:d w:200 k:p w:1500
scene ruler       UT_HOST_USB=1 w:500 k:d w:200 k:d w:200 k:d w:200 k:p w:1500 k:d w:300 k:p w:800
scene colours     UT_HOST_USB=1 w:500 k:d w:200 k:d w:200 k:d w:200 k:p w:1500 k:d w:300 k:d w:300 k:p w:500 k:u w:400
scene everyday    UT_HOST_USB=1 w:500 k:d w:200 k:d w:200 k:d w:200 k:d w:200 k:p w:600 k:p w:3400
scene pad         UT_HOST_USB=1 w:500 k:p w:800 k:d w:300 k:p w:800 t:700,900 w:300
scene files       UT_HOST_USB=1 w:500 k:d w:200 k:d w:200 k:p w:1200
scene files-hash  UT_HOST_USB=1 w:500 k:d w:200 k:d w:200 k:p w:1200 k:d w:200 k:d w:200 k:p w:4000
scene everyday4    UT_HOST_USB=1 w:500 k:d w:200 k:d w:200 k:d w:200 k:d w:200 k:p w:600 k:d w:200 k:d w:300
scene calc        UT_HOST_USB=1 w:500 k:d w:200 k:d w:200 k:d w:200 k:d w:200 k:p w:600 k:d w:200 k:d w:200 k:p w:600 t:450,2245 w:150 t:1290,1165 w:150 t:170,2245 w:150 t:170,2515 w:150 t:1290,2515 w:400
scene browse      UT_HOST_USB=1 w:500 k:d w:200 k:d w:200 k:p w:1200 k:d w:200 k:p w:1200
scene browse-dir  UT_HOST_USB=1 w:500 k:d w:200 k:d w:200 k:p w:1200 k:d w:200 k:p w:1200 k:d w:200 k:d w:200 k:p w:800
scene file-hash   UT_HOST_USB=1 w:500 k:d w:200 k:d w:200 k:p w:1200 k:d w:200 k:p w:1200 k:d w:200 k:d w:200 k:d w:200 k:d w:200 k:d w:200 k:d w:200 k:p w:1200 k:d w:200 k:p w:3000
scene file-text   UT_HOST_USB=1 w:500 k:d w:200 k:d w:200 k:p w:1200 k:d w:200 k:p w:1200 k:d w:200 k:d w:200 k:d w:200 k:d w:200 k:d w:200 k:d w:200 k:d w:200 k:p w:1200 k:d w:200 k:p w:1500
scene file-hex    UT_HOST_USB=1 w:500 k:d w:200 k:d w:200 k:p w:1200 k:d w:200 k:p w:1200 k:d w:200 k:d w:200 k:d w:200 k:d w:200 k:d w:200 k:d w:200 k:p w:1500
scene timer-awake UT_HOST_USB=1 w:500 k:d w:200 k:d w:200 k:d w:200 k:d w:200 k:p w:600 k:d w:200 k:p w:200 w:130000
scene saver       UT_HOST_USB=1 w:500 k:p w:800 w:125000
scene saver-wake  UT_HOST_USB=1 w:500 k:p w:800 w:125000 t:720,2170 w:800
scene idle-warn   UT_HOST_USB=0 w:550000
echo "PNGs in $png"
