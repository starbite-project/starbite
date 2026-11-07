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
    "$lib/assets.S" "$ut/main.c" "$ut/msc.c" "$ut/fat.c" -lm -lpthread

scene() {
    name=$1
    env=$2
    shift 2
    limactl shell --workdir "$root" starbite env $env "$out/ut-host" "$out" /dev/null "$@" "s:$name" q | grep -E "panel|FAIL|host:" || true
    "$py" -c "from PIL import Image; Image.open('$out/$name.ppm').save('$png/$name.png')"
}
scene home        UT_HOST_USB=0 w:500
scene home-usb    UT_HOST_USB=1 w:1500
scene usb         UT_HOST_USB=1 w:500 k:p w:800
scene radio       UT_HOST_USB=1 w:500 k:d w:200 k:p w:800
scene leave       UT_HOST_USB=1 w:500 k:u w:200 k:p w:800
scene idle-warn   UT_HOST_USB=0 w:150000
echo "PNGs in $png"
