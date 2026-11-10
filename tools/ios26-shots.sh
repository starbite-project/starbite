#!/usr/bin/env bash
# Pull sbca's display shots from iOS 26's card root and convert them to PNG (phone in TWRP, read only).
#   tools/ios26-shots.sh OUTDIR 090 120 150      seconds after the first frame (sbca writes 060..360)
# sbca (tools/ca-gpu/sbca.c, in backboardd; boot-arg sbshot=0 turns it off) writes every panel-sized BGRA
# surface as /private/var/tmp/sbca-shot-<s>-<id>-<W>x<H>-<rowbytes>.bgra. All surfaces of each listed second
# are pulled (a few, ~6 MB each) and written as OUTDIR/shot-<s>-<id>.png.
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "phone not in TWRP"; exit 1; }
out=${1:?OUTDIR}; shift
mkdir -p "$out"
python3 tools/ios26-find.py 'sbca-shot-*' /private/var/tmp --depth 1 > "$out/shots.txt"
for s in "$@"; do
    awk -v s="sbca-shot-$s-" '$1=="file" && index($NF, s) {print $NF}' "$out/shots.txt" | while IFS= read -r p; do
        f=$(basename "$p")
        HFS_CARD=1 python3 tools/overlay/hfs-read.py "$p" "$out/$f" </dev/null >/dev/null
        python3 - "$out/$f" <<'PY'
import re, struct, sys, zlib
p = sys.argv[1]
m = re.search(r'sbca-shot-(\d+)-(\d+)-(\d+)x(\d+)-(\d+)\.bgra$', p)
s, i, w, h, row = (int(x) for x in m.groups())
raw = open(p, 'rb').read()
lines = bytearray()
for y in range(h):
    r = raw[y * row:y * row + w * 4]
    lines += b'\0' + bytes(b for px in range(w) for b in (r[px * 4 + 2], r[px * 4 + 1], r[px * 4]))
def chunk(t, d):
    return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + \
    chunk(b'IDAT', zlib.compress(bytes(lines), 6)) + chunk(b'IEND', b'')
out = p.rsplit('/', 1)[0] + f'/shot-{s:03d}-{i}.png'
open(out, 'wb').write(png)
print(out)
PY
        rm -f "$out/$f"
    done
done
