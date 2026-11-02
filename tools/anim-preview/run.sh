#!/bin/sh
# Preview the boot intro (shim/starlte/anim.c) on the host, frame by frame: see anim_preview.c.
# Run on the Mac from anywhere in the repo:  sh tools/anim-preview/run.sh [ms ...]
#   AP_FRAME_US=66667   frames at 15 fps (what a slow phone would show); default 60 fps
#   AP_KEY_MS=500       a key goes down at 500 ms, up 300 ms later (AP_KEY_UP_MS): the skip path
#   AP_GAP_MS=300       the loader's and the menu's work between intro and reveal (default 40)
#   AP_A55X=12          also print the host frame times scaled by this factor
#   AP_OUT=dir          output directory (default build/anim-preview)
#   AP_HOST=1           build and run natively on the Mac instead of in the Lima VM (automatic
#                       when the VM does not answer): the sources are copied with the loader's RAM
#                       addresses moved up by HOSTOFF, since macOS cannot map below 4 GiB
# Needs build/boot-uc3/ramdisk-v6.bin (V6=1 menu sprites + font), build/menu6-preview/catalog.sbc
# and build/brand/upparam-starbite/logo.jpg; copies them from the main checkout when missing.
# Out: f*.png, final.png, sheet-*.png, intro.gif (the 1/8 size film is deleted after use)
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
main=$(git -C "$root" worktree list --porcelain | awk 'NR == 1 { print $2 }')
out=${AP_OUT:-"$root/build/anim-preview"}
mkdir -p "$out"
py="$root/build/venv/bin/python"
[ -x "$py" ] || py="$main/build/venv/bin/python"
for f in boot-uc3/ramdisk-v6.bin menu6-preview/catalog.sbc brand/upparam-starbite/logo.jpg; do
    [ -f "$root/build/$f" ] || { mkdir -p "$(dirname "$root/build/$f")"; cp "$main/build/$f" "$root/build/$f"; }
done
cp "$root/build/boot-uc3/ramdisk-v6.bin" "$root/build/menu6-preview/catalog.sbc" "$out/"
[ -f "$out/logo.rgb" ] || "$py" -I -c "
from PIL import Image
open('$out/logo.rgb', 'wb').write(Image.open('$root/build/brand/upparam-starbite/logo.jpg').convert('RGB').tobytes())"
times=${*:-0 150 300 450 600 700 760 820 900 980 1040 1100 1130 1160 1220 1300 1400 1550 1750 2000}
rm -f "$out"/f*.ppm "$out"/f*.png "$out"/frames.raw
env="AP_FRAME_US=${AP_FRAME_US:-16667} AP_GAP_MS=${AP_GAP_MS:-40} AP_A55X=${AP_A55X:-0}"
[ -n "${AP_KEY_MS:-}" ] && env="$env AP_KEY_MS=$AP_KEY_MS"
[ -n "${AP_KEY_UP_MS:-}" ] && env="$env AP_KEY_UP_MS=$AP_KEY_UP_MS"
if [ -z "${AP_HOST:-}" ] && ! limactl shell starbite -- true 2>/dev/null; then
    echo "the Lima VM does not answer: building natively on the Mac" >&2
    AP_HOST=1
fi
if [ -z "${AP_HOST:-}" ]; then
    limactl shell starbite -- bash -lc "set -e; cd '$out'; \
      gcc -c -O2 -ffreestanding -fno-builtin -mgeneral-regs-only -mstrict-align -Wall -Wextra -Werror \
        -I'$root/shim/starlte' -I'$root/shim' '$root/shim/starlte/anim.c' -o anim.o; \
      gcc -O2 -w -march=armv8-a+crc -I'$root/shim/starlte' -I'$root/shim' -o ap '$root/tools/anim-preview/anim_preview.c' \
        '$root/shim/starlte/ui.c' '$root/shim/starlte/catalog.c' anim.o; \
      env $env ./ap ramdisk-v6.bin catalog.sbc logo.rgb $times"
else
    src="$out/host-src"
    rm -rf "$src"
    mkdir -p "$src/starlte"
    cp "$root"/shim/*.h "$src/"
    cp "$root"/shim/starlte/*.c "$root"/shim/starlte/*.h "$src/starlte/"
    # the loader's fixed RAM (0x84000000..0x9fffffff) and the panel (0xcc000000), moved up
    sed -E -i '' 's/(0x(8[4-9a-f][0-9a-f]{6}|9[0-9a-f]{7}|cc000000)UL)/(\1 + HOSTOFF)/g' "$src"/starlte/*.c "$src"/starlte/*.h
    hostoff=0x3000000000UL
    clang -c -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror -DHOSTOFF=$hostoff \
        -I"$src/starlte" -I"$src" "$src/starlte/anim.c" -o "$out/anim.o"
    clang -O2 -w -DHOSTOFF=$hostoff -I"$src/starlte" -I"$src" -o "$out/ap" "$root/tools/anim-preview/anim_preview.c" \
        "$src/starlte/ui.c" "$src/starlte/catalog.c" "$out/anim.o"
    cat > "$out/ppm2png.py" <<'PY'
import os, sys
from PIL import Image
Image.open(sys.argv[1]).save(sys.argv[1][:-4] + '.png')
os.remove(sys.argv[1])
PY
    (cd "$out" && env $env AP_PPM2PNG="'$py' -I '$out/ppm2png.py'" ./ap ramdisk-v6.bin catalog.sbc logo.rgb $times)
fi
"$py" -I - "$out" <<'PY'
import struct, sys, glob, os
from PIL import Image, ImageDraw
out = sys.argv[1]
for p in sorted(glob.glob(os.path.join(out, '*.ppm'))):
    Image.open(p).save(p[:-4] + '.png')
    os.remove(p)
# every frame at 1/8 size: contact sheets (12 a sheet) and an animated GIF
W, H = 180, 370
raw = open(os.path.join(out, 'frames.raw'), 'rb').read()
n = len(raw) // (4 + W * H * 3)
frames = []
for i in range(n):
    o = i * (4 + W * H * 3)
    t = struct.unpack('<I', raw[o:o + 4])[0]
    frames.append((t, Image.frombytes('RGB', (W, H), raw[o + 4:o + 4 + W * H * 3])))
os.remove(os.path.join(out, 'frames.raw'))
for f in glob.glob(os.path.join(out, 'sheet-*.png')):
    os.remove(f)
per, cols = 12, 6
for s in range(0, n, per):
    sheet = Image.new('RGB', (cols * (W + 8), 2 * (H + 30)), (40, 40, 48))
    d = ImageDraw.Draw(sheet)
    for k, (t, im) in enumerate(frames[s:s + per]):
        x, y = (k % cols) * (W + 8), (k // cols) * (H + 30)
        sheet.paste(im, (x + 4, y + 26))
        d.text((x + 8, y + 6), f'{t} ms', fill=(255, 255, 255))
    sheet.save(os.path.join(out, f'sheet-{s // per:02d}.png'))
step = int(os.environ.get('AP_FRAME_US', '16667')) // 1000
frames[0][1].save(os.path.join(out, 'intro.gif'), save_all=True, append_images=[im for _, im in frames[1:]],
                  duration=max(step, 17), loop=0)
print(f'{n} frames -> {out}/sheet-*.png, intro.gif')
PY
