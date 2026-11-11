#!/usr/bin/env python3
# imgtest.py DIR: Photos' decoders (shell_media_img.inc: JPEG, PNG, the row filter) against PIL, on pictures drawn
# here (run.sh imgtest builds DIR/imgtest from tools/imgtest.c). At full size a 4:4:4 JPEG and every PNG must match
# PIL (max 3 / 0); 4:2:0 and 4:2:2 differ by PIL's smoother chroma; at exact 1/2, 1/4, 1/8 the reduced decode must
# match PIL's box filter (mean < 1); other sizes differ by the sampling grid. A progressive JPEG must be refused.
import os, subprocess, sys, io, struct
import numpy as np
from PIL import Image, ImageDraw

d = sys.argv[1]; exe = os.path.join(d, 'imgtest')
rng = np.random.default_rng(7)

def pic(w, h):
    y, x = np.mgrid[0:h, 0:w]
    a = np.stack([127 + 120 * np.sin(x / 37.0 + y / 91.0), 127 + 120 * np.cos(y / 53.0), 127 + 120 * np.sin((x + y) / 71.0)], -1)
    im = Image.fromarray(a.clip(0, 255).astype(np.uint8))
    dr = ImageDraw.Draw(im)
    for i in range(12):
        x0, y0 = int(rng.integers(0, w - 40)), int(rng.integers(0, h - 40))
        dr.rectangle([x0, y0, x0 + int(rng.integers(20, w // 3)), y0 + int(rng.integers(20, h // 3))], outline=tuple(int(v) for v in rng.integers(0, 255, 3)), width=5)
    dr.ellipse([w // 4, h // 4, w // 2, h // 2], fill=(250, 240, 20))
    return im

def run(path, tw, th, thumb=False):
    out = os.path.join(d, 'o.ppm')
    r = subprocess.run([exe, path, out, str(tw), str(th)] + (['t'] if thumb else []), capture_output=True, text=True)
    return r.stdout.strip().replace('\n', '; '), (np.asarray(Image.open(out).convert('RGB')).astype(float) if r.returncode == 0 else None)

def cmp(name, path, ref, tw, th, **kw):
    info, got = run(path, tw, th, **kw)
    if got is None:
        print(f'{name}: {info}'); return
    want = np.asarray(ref.resize((got.shape[1], got.shape[0]), Image.BOX).convert('RGB')).astype(float)
    print(f'{name}: {info}; out {got.shape[1]}x{got.shape[0]}; mean abs diff {np.abs(got - want).mean():.2f}, max {np.abs(got - want).max():.0f}')

src = pic(1203, 907)
for sub, nm in ((0, '444'), (1, '422'), (2, '420')):
    p = os.path.join(d, f'a{nm}.jpg'); src.save(p, quality=88, subsampling=sub)
    ref = Image.open(p)
    for tw, th in ((1203, 907), (600, 450), (300, 226), (150, 113), (97, 61)):
        cmp(f'jpeg {nm} -> {tw}x{th}', p, ref, tw, th)
p = os.path.join(d, 'rst.jpg'); src.save(p, quality=75, restart_marker_blocks=7); cmp('jpeg restart', p, Image.open(p), 1203, 907)
p = os.path.join(d, 'grey.jpg'); src.convert('L').save(p, quality=80); cmp('jpeg grey', p, Image.open(p), 400, 300)
p = os.path.join(d, 'opt.jpg'); src.save(p, quality=95, optimize=True); cmp('jpeg optimised huffman', p, Image.open(p), 1203, 907)
p = os.path.join(d, 'prog.jpg'); src.save(p, progressive=True); cmp('jpeg progressive', p, Image.open(p), 100, 100)
tb = io.BytesIO(); src.resize((320, 240)).save(tb, 'JPEG', quality=80)
p = os.path.join(d, 'exif.jpg')
def exif_with_thumb(orient, thumb):
    ifd0 = struct.pack('<H', 1) + struct.pack('<HHII', 0x0112, 3, 1, orient) + struct.pack('<I', 8 + 2 + 12 + 4)
    off = 8 + len(ifd0) + 2 + 24 + 4
    ifd1 = struct.pack('<H', 2) + struct.pack('<HHII', 0x0201, 4, 1, off) + struct.pack('<HHII', 0x0202, 4, 1, len(thumb)) + struct.pack('<I', 0)
    return b'Exif\0\0' + b'II*\0' + struct.pack('<I', 8) + ifd0 + ifd1 + thumb
src.save(p, quality=85, exif=exif_with_thumb(6, tb.getvalue()))
ref = Image.open(p).transpose(Image.ROTATE_270)
cmp('jpeg exif orient 6', p, ref, 600, 450)
cmp('jpeg exif thumb', p, Image.open(io.BytesIO(tb.getvalue())).transpose(Image.ROTATE_270), 240, 180, thumb=True)
for mode, nm in (('RGB', 'rgb'), ('RGBA', 'rgba'), ('L', 'grey'), ('P', 'pal'), ('1', 'bw'), ('LA', 'la')):
    im = src.convert(mode)
    p = os.path.join(d, f'p{nm}.png'); im.save(p)
    cmp(f'png {nm}', p, Image.open(p).convert('RGB'), 1203, 907)
    cmp(f'png {nm} small', p, Image.open(p).convert('RGB'), 300, 226)
p = os.path.join(d, 'p0.png'); src.save(p, compress_level=0); cmp('png stored', p, Image.open(p), 1203, 907)
big = pic(1440, 2960); p = os.path.join(d, 'shot.png'); big.save(p); cmp('png screenshot size', p, Image.open(p), 720, 1480)

# exact reductions: the decoder's 1/2, 1/4, 1/8 against PIL's box filter of the full decode
y, x = np.mgrid[0:896, 0:1200]
a = np.stack([127 + 120 * np.sin(x / 37.0 + y / 91.0), 127 + 120 * np.cos(y / 53.0), 127 + 120 * np.sin((x + y) / 71.0)], -1).clip(0, 255).astype(np.uint8)
a[200:500, 300:700] = (250, 240, 20)
p = os.path.join(d, 'exact.jpg'); Image.fromarray(a).save(p, quality=90, subsampling=0)
bad = 0
for k in (2, 4, 8):
    info, got = run(p, 1200 // k, 896 // k)
    want = np.asarray(Image.open(p).resize((1200 // k, 896 // k), Image.BOX)).astype(float)
    m = np.abs(got - want).mean()
    bad += m >= 1
    print(f'jpeg exact 1/{k}: mean abs diff {m:.2f}, max {np.abs(got - want).max():.0f}')
print('imgtest:', 'FAILED' if bad else 'the exact reductions match')
sys.exit(1 if bad else 0)
