#!/bin/sh
# Dump the geometry of every visible element of the approved boot concept
# (docs/concepts/ios27-boot.html) per state, in panel px, for menu v7's builders and
# its preview diff (docs/MENU7-PORT.md section 7):
#   docs/concepts/boot-geometry/<state>.json   one object per element:
#     p path, r {x,y,w,h} box, a opacity (product of ancestors), f data-f focus key,
#     glass {r,clip,frost,tint,s} for data-glass shapes, text + base (baseline y) + tx (pen x)
#     + tw (advance) + font "family weight size ls lh" + color, tf CSS transform, bg/br/sh boxes.
#   build/menu7/concept-shots/<state>.png      the concept rendered at 1000 x 1500, solo (not committed)
# Needs Google Chrome (headless) and network access for Google Fonts.
#   sh tools/menu7/concept-geometry.sh [state ...]
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
chrome="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
sed 's#</body></html>##' "$root/docs/concepts/ios27-boot.html" > "$tmp/c.html"
cat "$root/tools/menu7/concept-dump.js.html" >> "$tmp/c.html"
echo '</body></html>' >> "$tmp/c.html"
mkdir -p "$root/docs/concepts/boot-geometry" "$root/build/menu7/concept-shots"
states=${*:-"menu countdown collapsed apple ctx submenu sheet alert recovery launch error handoff missing lab intro-0 intro-400 intro-800 intro-1100 intro-1400 intro-1700 intro-2000"}
for st in $states; do
    case $st in
    intro-*) hash="state=intro&t=${st#intro-}&solo" ;;
    *) hash="state=$st&snap&solo" ;;
    esac
    "$chrome" --headless=new --window-size=1000,1500 --virtual-time-budget=9000 --dump-dom \
        "file://$tmp/c.html#$hash" 2>/dev/null | python3 -c '
import sys, re, html, json
s = sys.stdin.read()
m = re.search(r"<pre id=\"DUMP\">(.*?)</pre>", s, re.S)
d = json.loads(html.unescape(m.group(1)))
with open(sys.argv[1], "w") as f:
    f.write("[\n" + ",\n".join(json.dumps(o, separators=(",", ":")) for o in d) + "\n]\n")
print(sys.argv[1], len(d), "elements")' "$root/docs/concepts/boot-geometry/$st.json"
    "$chrome" --headless=new --window-size=1000,1500 --virtual-time-budget=6000 \
        --screenshot="$root/build/menu7/concept-shots/$st.png" \
        "file://$root/docs/concepts/ios27-boot.html#$hash" 2>/dev/null >/dev/null || true
done
