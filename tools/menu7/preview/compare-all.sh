#!/bin/sh
# Every concept state against the loader's own frame for it (menu7.c + intro7.c + glass*.c via h7).
# Run on the Mac from anywhere in the repo:
#   sh tools/menu7/preview/compare-all.sh [state ...]        (default: all of them)
# For each state it writes into build/menu7-compare/:
#   concept/<s>.png   headless Chrome, docs/concepts/ios27-boot.html#state=<s>&snap&solo, 1000 x 1500
#   h7/<s>.png        h7 --state <s> (1440 x 2960, the panel)
#   <s>.png           concept | loader | |diff| x 4 at the concept's scale (diff.py), with
#                     per-region mean errors printed
# and all.png, every pair at a glance. 
#   CMP_SRC=dir    source tree for h7 (default this checkout; a private copy under build/menu7-work/)
#   CMP_NOCONCEPT=1   reuse concept shots already rendered
#   CMP_RD=file    the BOOT ramdisk h7 draws from (default build/brand/menu-v7.sbm, the real logos).
#                  For the concept's tile and provider-glyph artwork, build it with
#                  menu7-assets.py --concept-art and pass build/brand/concept/menu-v7.sbm
set -eu
root=$(cd "$(dirname "$0")/../../.." && pwd)
src=${CMP_SRC:-$root}
rd=${CMP_RD:-$root/build/brand/menu-v7.sbm}
case $rd in /*) ;; *) rd="$root/$rd" ;; esac
out="$root/build/menu7-compare"
py="$root/build/venv/bin/python"
pv="$root/tools/menu7/preview"
chrome="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
mkdir -p "$out/concept" "$out/h7"
states=${*:-"menu countdown collapsed apple ctx submenu sheet alert recovery launch error missing lab handoff intro-0 intro-300 intro-700 intro-1050 intro-1400 intro-1800 intro-2350"}

H7_SRC=$src H7_OUT="$out/bin" sh "$pv/run.sh" build >/dev/null

h7args() {   # state -> h7 arguments
    case $1 in
    intro-*) echo "--state intro --t ${1#intro-}" ;;
    launch) echo "--state launch --p 0.4" ;;
    *) echo "--state $1" ;;
    esac
}
hash() {     # state -> concept URL fragment
    case $1 in
    intro-*) echo "state=intro&stopat=${1#intro-}&solo" ;;
    error) echo "state=error&shotat=4500&solo" ;;   # real time, once the card has turned into the error
    launch) echo "state=launch&snap&p=0.4&solo" ;;
    missing|lab) echo "state=$1&shotat=1400&solo" ;; # real time, while the notice is up
    ctx|submenu|sheet|alert|recovery|apple|countdown) echo "state=$1&settle=700&solo" ;;  # real time, at rest
    *) echo "state=$1&snap&solo" ;;
    esac
}
# Headless Chrome runs almost no animation frames under virtual time (2 to 4 in 20 s): the menu
# states are fine, since #snap snaps every spring, but a frozen intro (#state=intro&t=T) holds the
# intro's transforms at T with the springs beside it (the list's room for the island) not yet moved.
# So the shots use a copy of the page (concept/.page.html) where
#  - #stopat=T stops the intro, run from state=intro in real time (cdp-shot.mjs, no virtual time:
#    animation frames then run at the display rate), at exactly T ms with every spring where the running intro has it,
#    which is what the loader shows at T;
#  - #shotat=MS (missing, lab: their notice and its room run on timers and springs) shoots in real
#    time MS after the state starts;
#  - #settle=MS (the overlay states, apple, countdown: their springs barely move under virtual time,
#    so the lens, the button rims and the dock under the blur were caught mid-flight) shoots in real
#    time once MS have passed and every spring has been at rest for 6 frames; the countdown's clock
#    is restarted 3 frames before, so its ring is full as in the loader's state;
#  - the end screens' 0.5 s CSS fade, which virtual time never runs, is already done.
"$py" -I - "$root/docs/concepts/ios27-boot.html" "$out/concept/.page.html" <<'PY'
import sys
s = open(sys.argv[1]).read()
def sub(a, b):
    global s
    assert s.count(a) == 1, a
    s = s.replace(a, b)
sub("const t=S.freeze!==null?S.freeze:now-S.t0;",
    "const STOP=+(HP.get('stopat')||1e9),t=S.freeze!==null?S.freeze:Math.min(now-S.t0,STOP);"
    "if(t>=STOP&&S.freeze===null){introAt(t);SPR.forEach(s=>s.step(dt));update();render();document.title='M7STOP';return}")
sub("else if(st){go(st);", "else if(st){go(st);if(HP.has('shotat'))setTimeout(()=>{document.title='M7STOP'},+HP.get('shotat'));"
    "if(HP.has('settle')){const T0=performance.now();let calm=0;const chk=()=>{"
    "const rest=[...SPR].every(q=>q.x===q.t&&q.v===0);"
    "calm=rest&&performance.now()-T0>+HP.get('settle')?calm+1:0;"
    "if(calm===3&&S.cd)S.cd.t0=NOW;"
    "if(calm>=6){document.title='M7STOP';return}requestAnimationFrame(chk)};requestAnimationFrame(chk)}")
sub('endEl.style.opacity=0;requestAnimationFrame(()=>endEl.style.opacity=1)', 'endEl.style.opacity=1')
open(sys.argv[2], 'w').write(s)
PY


cmds=""
for s in $states; do
    if [ -n "${CMP_NOCONCEPT:-}" ] && [ -f "$out/concept/$s.png" ]; then
        :
    elif [ "${s#intro-}" != "$s" ] || [ "$(hash "$s" | grep -c 'shotat\|settle\|stopat')" != 0 ]; then
        node "$pv/cdp-shot.mjs" "file://$out/concept/.page.html#$(hash "$s")" "$out/concept/$s.png"
    else
        "$chrome" --headless=new --window-size=1000,1500 --virtual-time-budget=6000 --hide-scrollbars \
            --screenshot="$out/concept/$s.png" "file://$out/concept/.page.html#$(hash "$s")" >/dev/null 2>&1
    fi
    cmds="$cmds ./h7 --ramdisk '$rd' $(h7args "$s") --out '$out/h7/$s.ppm' >/dev/null &&"
done
limactl shell starbite -- bash -lc "set -e; cd '$out/bin'; $cmds true"

"$py" -I - "$out" "$pv/diff.py" "$root" $states <<'PY'
import subprocess, sys
from pathlib import Path
from PIL import Image
out, diff, root = Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3])
states = sys.argv[4:]
cells = []
for s in states:
    ppm = out / 'h7' / f'{s}.ppm'
    png = out / 'h7' / f'{s}.png'
    Image.open(ppm).save(png)
    ppm.unlink()
    g = 'intro' if s.startswith('intro-') else s
    args = [sys.executable, '-I', diff, str(out / 'concept' / f'{s}.png'), str(png), str(out / f'{s}.png')]
    geom = root / 'docs/concepts/boot-geometry' / f'{s}.json'
    if geom.exists():
        args += ['--geom', str(geom)]
    r = subprocess.run(args, capture_output=True, text=True)
    print(f'== {s}\n{r.stdout.strip()}{r.stderr.strip()}')
    cells.append(s)
cw, ch = 666 * 2 // 3, 1369 // 3
sheet = Image.new('RGB', (cw * 2 * 4 + 30, ((len(cells) + 3) // 4) * (ch + 10)), (24, 24, 28))
for i, s in enumerate(cells):
    im = Image.open(out / f'{s}.png').convert('RGB')
    im = im.crop((0, 0, im.width * 2 // 3, im.height)).resize((cw * 2, ch), Image.LANCZOS)
    sheet.paste(im, ((i % 4) * (cw * 2 + 10), (i // 4) * (ch + 10)))
sheet.save(out / 'all.png')
print(f'{out}/all.png')
PY
