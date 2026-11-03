#!/bin/sh
# Multi-frame motion check (docs/MENU7-BUILD.md 6.1): the menu's section cards moved over N
# consecutive frames (no rest), the last frame against an exact render of the end position.
# Needs h7 built (run.sh build). H7_OUT / H7_RD as in run.sh.
#   sh tools/menu7/preview/motion-check.sh ["N STEP" ...]
set -eu
root=$(cd "$(dirname "$0")/../../.." && pwd)
out=${H7_OUT:-$root/build/menu7/preview}
rd=${H7_RD:-$root/build/brand/menu-v7.sbm}
py="$root/build/venv/bin/python"
pv="$root/tools/menu7/preview"
[ -x "$out/h7" ] || { echo "motion-check: build h7 first (run.sh build)" >&2; exit 2; }
"$py" -I "$pv/geom2dl.py" --state menu "$root/docs/concepts/boot-geometry/menu.json" > "$out/mc-menu.dl"
[ $# -gt 0 ] || set -- "1 176" "8 22" "22 8" "44 4" "40 -4.4" "60 0.3"
for cfg in "$@"; do
    set -- $cfg
    "$py" -I "$pv/motion.py" lists "$out/mc-menu.dl" "$1" "$2" "$out/mc-A.dl" "$out/mc-B.dl"
    D=$("$py" -c "print($1 * $2)")
    limactl shell starbite -- bash -lc "cd '$out' && ./h7 --ramdisk '$rd' --list mc-A.dl --out mc-A.ppm >/dev/null 2>&1 && ./h7 --ramdisk '$rd' --list mc-B.dl --out mc-B.ppm >/dev/null 2>&1"
    echo "N=$1 step=$2: $("$py" -I "$pv/motion.py" err "$out/mc-A.ppm" "$out/mc-B.ppm" "$D")"
done
rm -f "$out/mc-A.ppm" "$out/mc-B.ppm"
