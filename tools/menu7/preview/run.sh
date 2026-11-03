#!/bin/sh
# Menu v7 host preview (docs/MENU7-PORT.md section 5): build h7 in the Lima VM from the real
# glass*.c / sb7.c (and menu7*.c, intro7.c, loader_stubs.c when they exist), run it, and turn its
# PPM into PNG on the Mac.
#
#   sh tools/menu7/preview/run.sh build
#   sh tools/menu7/preview/run.sh geom <state> [h7 args]     concept geometry -> display list ->
#                                                            h7 -> PNG -> diff.py against the concept
#   sh tools/menu7/preview/run.sh list <scene.dl> <out.png> [h7 args]
#   sh tools/menu7/preview/run.sh state <state> <out.png> [h7 args]      (needs menu7)
#   (h7 --run [loader_stubs options]: menu7_main on the fake clock, each g7_frame timed: "T7 ...")
#
# H7_SRC   the source root (default: this checkout); private copies under build/menu7-work/<who>
# H7_OUT   output directory (default: build/menu7/preview)
# H7_RD    the BOOT ramdisk with SB7 (default: build/brand/menu-v7.sbm)
set -eu
root=$(cd "$(dirname "$0")/../../.." && pwd)
src=${H7_SRC:-$root}
out=${H7_OUT:-$root/build/menu7/preview}
rd=${H7_RD:-$root/build/brand/menu-v7.sbm}
py="$root/build/venv/bin/python"
mkdir -p "$out"
st="$src/shim/starlte"
pv="$src/tools/menu7/preview"
[ -d "$pv" ] || pv="$root/tools/menu7/preview"

build() {
    files="$st/glass.c $st/sb7.c"
    for f in "$st"/glass_*.c; do [ -f "$f" ] && files="$files $f"; done
    defs="-DM7_PREVIEW" wrap=""
    if [ -f "$st/menu7.c" ] && [ -f "$pv/loader_stubs.c" ]; then
        for f in "$st"/menu7*.c "$st/intro7.c" "$pv/loader_stubs.c"; do [ -f "$f" ] && files="$files $f"; done
        defs="$defs -DH7_MENU7 -DH7_TIME"; wrap="-Wl,--wrap=g7_frame -Wl,--wrap=g7_idle"
    fi
    limactl shell starbite -- bash -lc "set -e; cd '$out'; gcc -O2 -mcpu=cortex-a55 -fno-math-errno -ffp-contract=fast \
        -mstrict-align -Wall -Wextra -Werror -Wno-unused-parameter $defs -I'$st' -I'$src/shim' -o h7 '$pv/h7.c' $files $wrap -lm"
    echo "built $out/h7 ($defs)"
}
topng() {
    "$py" -I -c "
import sys
from PIL import Image
Image.open(sys.argv[1]).save(sys.argv[2])" "$1" "$2"
    rm -f "$1"
}
run_h7() {   # out.png args...
    png=$1; shift
    limactl shell starbite -- bash -lc "cd '$out' && ./h7 --ramdisk '$rd' --out '$out/h7.ppm' $*"
    topng "$out/h7.ppm" "$png"
}

cmd=${1:-build}
[ $# -gt 0 ] && shift
case $cmd in
build) build ;;
list) dl=$(cd "$(dirname "$1")" && pwd)/$(basename "$1") png=$2; shift 2; [ -x "$out/h7" ] || build; run_h7 "$png" --list "$dl" "$@" ;;
state) s=$1 png=$2; shift 2; [ -x "$out/h7" ] || build; run_h7 "$png" --state "$s" "$@" ;;
geom)
    s=$1; shift
    [ -x "$out/h7" ] || build
    "$py" -I "$pv/geom2dl.py" --state "$s" "$root/docs/concepts/boot-geometry/$s.json" > "$out/$s.dl"
    run_h7 "$out/$s-glass.png" --list "$out/$s.dl" "$@"
    "$py" -I "$pv/diff.py" "$root/build/menu7/concept-shots/$s.png" "$out/$s-glass.png" "$out/$s-diff.png" --state "$s"
    ;;
*) echo "run.sh: build | geom <state> | list <dl> <png> | state <state> <png>" >&2; exit 2 ;;
esac
