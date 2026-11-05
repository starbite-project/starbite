#!/bin/sh
# Engineer OS host preview (docs/ENGINEER-OS.md section 7). Run on the Mac from the repo root:
#   sh starbite/host/preview.sh            build in the Lima VM, run every scene, PNGs to
#                                          starbite/engineer/preview/
#   sh starbite/host/preview.sh SCENE      one scene
# The VM compiles the real Engineer OS and sbui sources for aarch64 Linux with starbite/host/host.c
# underneath (fake hardware, scripted keys and taps on a fake clock); the Mac converts the PPM
# frames with build/venv's Pillow. Each shot also prints how many panel pixels differ from the
# back buffer: 0 means every change was presented (the dirty rectangles cover what was drawn).
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
out="$root/starbite/out/host"
png="$root/starbite/engineer/preview"
py="$root/build/venv/bin/python"
[ -x "$py" ] || py="$root/../../../build/venv/bin/python"
mkdir -p "$out" "$png"
lib="$root/starbite/libsb"
eng="$root/starbite/engineer"
SB7=""
for c in "$root/build/brand/menu7.sb7" "$root/../../../build/brand/menu7.sb7"; do
    if [ -f "$c" ]; then SB7=$c; break; fi
done
[ -n "$SB7" ] || { echo "no build/brand/menu7.sb7"; exit 1; }

limactl shell --workdir "$root" starbite python3 starbite/tools/mkassets.py "$SB7" "$out/sbassets.sb7"
limactl shell --workdir "$root" starbite python3 tools/store/sbstore2.py catalog tools/store/catalog.toml "$out/catalog.sbc"
limactl shell --workdir "$root" starbite gcc -O1 -g -DSB_HOST "-DSB_ASSETS=\"$out/sbassets.sb7\"" \
    -Wall -Wextra -Werror -Wno-unused-parameter -fno-strict-aliasing -I"$lib" -I"$eng" \
    -o "$out/engineer-host" "$root/starbite/host/host.c" \
    "$lib/str.c" "$lib/log.c" "$lib/sbui.c" "$lib/sbui_input.c" "$lib/sbui_page.c" "$lib/sbui_w.c" "$lib/sb7.c" \
    "$lib/fdt.c" "$lib/catalog.c" "$lib/fence.c" "$lib/tmu.c" "$lib/assets.S" "$eng"/*.c -lm

# scene name, then the script (taps: tab bar at y 2824, tab t at x 90 + 180 t)
scene() {
    name=$1
    shift
    limactl shell --workdir "$root" starbite "$out/engineer-host" "$out" "$out/catalog.sbc" "$@" "s:$name" q
    "$py" -I -c "
import sys
from PIL import Image
Image.open(sys.argv[1]).save(sys.argv[2], optimize=True)" "$out/$name.ppm" "$png/$name.png"
    rm -f "$out/$name.ppm"
}

all="dashboard tests power battery clocks temps storage partitions starbite catalog start logs pmsg tools armed change peek device soc memory history dialog sector gpt keys"
want=${1:-$all}
for s in $want; do
    case $s in
    dashboard) scene dashboard w:90000 ;;
    tests) scene tests w:800 t:270,2824 w:500 ;;
    power) scene power w:800 t:450,2824 w:500 ;;
    battery) scene battery w:800 t:450,2824 w:400 k:d k:p w:2300 ;;
    clocks) scene clocks w:800 t:630,2824 w:500 ;;
    temps) scene temps w:800 t:630,2824 w:400 k:d k:d k:d k:p w:1500 ;;
    storage) scene storage w:800 t:810,2824 w:500 ;;
    partitions) scene partitions w:800 t:810,2824 w:400 k:d k:d k:d k:p w:800 ;;
    starbite) scene starbite w:800 t:990,2824 w:500 ;;
    catalog) scene catalog w:800 t:990,2824 w:400 k:d k:d k:p w:800 ;;
    start) scene start w:800 t:990,2824 w:400 k:d k:d k:d k:d k:p w:800 ;;
    logs) scene logs w:800 t:1170,2824 w:500 ;;
    pmsg) scene pmsg w:800 t:1170,2824 w:400 k:d k:d k:p w:800 ;;
    tools) scene tools w:800 t:1350,2824 w:500 ;;
    armed) scene armed w:800 t:1350,2824 w:400 k:d k:p w:600 k:p k:p w:400 h:p:2300 w:300 h:u:1800 w:900 ;;
    change) scene change w:800 t:1350,2824 w:400 k:d k:p w:600 k:p k:p w:400 h:p:2300 w:300 h:u:1800 w:900 k:d k:d k:p w:700 ;;
    peek) scene peek w:800 t:1350,2824 w:400 k:d k:d k:p w:800 ;;
    device) scene device w:800 k:d k:p w:800 ;;
    soc) scene soc w:800 k:d k:d k:p w:800 ;;
    memory) scene memory w:800 k:d k:d k:d k:p w:800 g:700,2000,-900,300 w:800 S:memory-scroll1 g:700,1200,500,200 w:1500 S:memory-scroll2 k:d k:d w:300 ;;
    history) scene history w:800 t:270,2824 w:400 k:d k:d k:d k:d k:d k:d k:d k:d k:d k:d k:p w:800 ;;
    dialog) scene dialog w:800 t:270,2824 w:400 k:d k:p w:600 ;;
    sector) scene sector w:800 t:810,2824 w:400 k:d k:d k:d k:d k:d k:p w:800 ;;
    gpt) scene gpt w:800 t:810,2824 w:400 k:d k:d k:d k:d k:p w:800 ;;
    keys) scene keys w:800 t:270,2824 w:400 k:d k:d k:d k:d k:d k:d k:d k:d k:p w:600 ;;
    *) echo "unknown scene $s"; exit 1 ;;
    esac
done
