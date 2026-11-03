#!/bin/sh
# The intro of menu v7 (shim/starlte/intro7.c) against the concept, frame by frame.
# Run on the Mac from anywhere in the repo:  sh tools/menu7/preview/intro-film.sh [ms ...]
#
# For each t (default: docs/MENU7-PORT.md section 5's list) it renders
#   - the concept, frozen at t (headless Chrome: #state=intro&t=<ms>&snap&solo, 1000 x 1500)
#   - the loader's frame: h7 --state intro --t <ms> (intro7_frame + glass), in the Lima VM
# and writes them side by side into one sheet (concept above, h7 below), plus, when
# tools/menu7/preview/diff.py exists, its per-region diff of every pair.
#   I7_H7=path       the h7 binary (default tools/menu7/preview/h7, built by run.sh)
#   I7_RAMDISK=path  the BOOT ramdisk with the SB7 container (default build/brand/menu-v7.sbm)
#   I7_OUT=dir       output directory (default build/menu7/intro-film)
#   I7_NOCONCEPT=1   reuse the concept shots already in I7_OUT
# Out: concept-<t>.png, h7-<t>.png, sheet.png (and diff-<t>.* from diff.py). A few PNGs only.
set -eu
root=$(cd "$(dirname "$0")/../../.." && pwd)
out=${I7_OUT:-"$root/build/menu7/intro-film"}
h7=${I7_H7:-"$root/tools/menu7/preview/h7"}
rd=${I7_RAMDISK:-"$root/build/brand/menu-v7.sbm"}
chrome="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
py="$root/build/venv/bin/python"
[ -x "$py" ] || py=python3
times=${*:-0 200 400 600 800 1000 1100 1250 1400 1550 1700 1850 2000 2350}
mkdir -p "$out"

for t in $times; do
    if [ -z "${I7_NOCONCEPT:-}" ] || [ ! -f "$out/concept-$t.png" ]; then
        "$chrome" --headless=new --window-size=1000,1500 --virtual-time-budget=6000 \
            --screenshot="$out/concept-$t.png" \
            "file://$root/docs/concepts/ios27-boot.html#state=intro&t=$t&snap&solo" >/dev/null 2>&1
    fi
done

have_h7=0
if [ -x "$h7" ] && [ -f "$rd" ]; then
    have_h7=1
    cmds=""
    for t in $times; do
        cmds="$cmds '$h7' --ramdisk '$rd' --state intro --t $t --out '$out/h7-$t.png' --stats &&"
    done
    limactl shell starbite -- bash -lc "set -e; cd '$out'; $cmds true"
else
    echo "intro-film: no h7 ($h7) or no ramdisk ($rd): concept frames only" >&2
fi

"$py" -I - "$out" "$have_h7" $times <<'PY'
import sys
from pathlib import Path
from PIL import Image
out, have = Path(sys.argv[1]), sys.argv[2] == '1'
ts = sys.argv[3:]
cw, ch = 250, 375                       # one cell: the concept window at 1 / 4
rows = 2 if have else 1
cols = min(len(ts), 7)
lines = (len(ts) + cols - 1) // cols
sheet = Image.new('RGB', (cols * cw, lines * rows * ch), (24, 24, 28))
for i, t in enumerate(ts):
    x, y = (i % cols) * cw, (i // cols) * rows * ch
    sheet.paste(Image.open(out / f'concept-{t}.png').convert('RGB').resize((cw, ch)), (x, y))
    if have:
        p = out / f'h7-{t}.png'
        if p.exists():
            im = Image.open(p).convert('RGB')
            h = ch - 8
            w = round(im.width * h / im.height)
            sheet.paste(im.resize((w, h), Image.LANCZOS), (x + (cw - w) // 2, y + ch + 4))
sheet.save(out / 'sheet.png')
print(f'{out}/sheet.png: {len(ts)} times' + ('' if have else ' (concept only)'))
PY

if [ "$have_h7" = 1 ] && [ -f "$root/tools/menu7/preview/diff.py" ]; then
    for t in $times; do
        "$py" -I "$root/tools/menu7/preview/diff.py" "$out/concept-$t.png" "$out/h7-$t.png" "$out/diff-$t" || true
    done
fi
