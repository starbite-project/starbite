#!/bin/sh
# Preview the boot intro (shim/starlte/anim.c) on the host, frame by frame: see anim_preview.c.
# Run on the Mac from anywhere in the repo:  sh tools/anim-preview/run.sh [ms ...]
# Needs build/boot-uc3/ramdisk-v6.bin (V6=1 menu sprites + font), build/menu6-preview/catalog.sbc
# and build/brand/upparam-starbite/logo.jpg; copies them from the main checkout when missing.
# Out: build/anim-preview/{f*.png, final.png, sheet-*.png, intro.gif}
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
main=$(git -C "$root" worktree list --porcelain | awk 'NR == 1 { print $2 }')
out="$root/build/anim-preview"
mkdir -p "$out"
py="$root/build/venv/bin/python"
[ -x "$py" ] || py="$main/build/venv/bin/python"
for f in boot-uc3/ramdisk-v6.bin menu6-preview/catalog.sbc brand/upparam-starbite/logo.jpg; do
    [ -f "$root/build/$f" ] || { mkdir -p "$(dirname "$root/build/$f")"; cp "$main/build/$f" "$root/build/$f"; }
done
"$py" -I -c "
from PIL import Image
Image.open('$root/build/brand/upparam-starbite/logo.jpg').convert('RGB').save('$out/logo.ppm')
open('$out/logo.rgb', 'wb').write(Image.open('$out/logo.ppm').tobytes())"
times=${*:-0 150 300 450 600 800 900 1000 1150 1250 1350 1450 1550 1650 1800 1950 2150 2400}
rm -f "$out"/f*.ppm "$out"/f*.png
limactl shell starbite -- bash -lc "set -e; cd '$out'; \
  gcc -c -O2 -ffreestanding -fno-builtin -mgeneral-regs-only -mstrict-align -Wall -Wextra -Werror \
    -I'$root/shim/starlte' -I'$root/shim' '$root/shim/starlte/anim.c' -o anim.o; \
  gcc -O2 -w -march=armv8-a+crc -I'$root/shim/starlte' -I'$root/shim' -o ap '$root/tools/anim-preview/anim_preview.c' \
    '$root/shim/starlte/ui.c' '$root/shim/starlte/catalog.c' anim.o; \
  ./ap ramdisk-v6.bin catalog.sbc logo.rgb $times"
"$py" -I - "$out" <<'PY'
import struct, sys, glob, os
from PIL import Image, ImageDraw
out = sys.argv[1]
for p in sorted(glob.glob(os.path.join(out, '*.ppm'))):
    if p.endswith('logo.ppm'):
        continue
    Image.open(p).save(p[:-4] + '.png')
    os.remove(p)
# every frame at 1/4 size: contact sheets (12 a sheet) and an animated GIF
W, H = 360, 740
raw = open(os.path.join(out, 'frames.raw'), 'rb').read()
n = len(raw) // (4 + W * H * 3)
frames = []
for i in range(n):
    o = i * (4 + W * H * 3)
    t = struct.unpack('<I', raw[o:o + 4])[0]
    frames.append((t, Image.frombytes('RGB', (W, H), raw[o + 4:o + 4 + W * H * 3])))
for f in glob.glob(os.path.join(out, 'sheet-*.png')):
    os.remove(f)
per, cols = 12, 6
for s in range(0, n, per):
    sheet = Image.new('RGB', (cols * (W // 2 + 8), 2 * (H // 2 + 30)), (40, 40, 48))
    d = ImageDraw.Draw(sheet)
    for k, (t, im) in enumerate(frames[s:s + per]):
        x, y = (k % cols) * (W // 2 + 8), (k // cols) * (H // 2 + 30)
        sheet.paste(im.resize((W // 2, H // 2), Image.BOX), (x + 4, y + 26))
        d.text((x + 8, y + 6), f'{t} ms', fill=(255, 255, 255))
    sheet.save(os.path.join(out, f'sheet-{s // per:02d}.png'))
frames[0][1].save(os.path.join(out, 'intro.gif'), save_all=True, append_images=[im for _, im in frames[1:]],
                  duration=17, loop=0)
print(f'{n} frames -> {out}/sheet-*.png, intro.gif')
PY
