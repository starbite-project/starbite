#!/usr/bin/env python3
"""Samsung's own boot splash without "Secured by Knox", for UP_PARAM (S-Boot draws logo.jpg 1:1).

    build/venv/bin/python -I tools/brand/splash-noknox.py [STOCK_UP_PARAM.img] [OUT_DIR]

defaults: backup/20261102-185306/UP_PARAM.img (S9 #1 before any splash was written: byte-identical
to the VG6 firmware's up_param.bin for its first 1402880 bytes, then zeros), build/brand/upparam-native.

The stock logo.jpg (1079 x 1747, Photoshop baseline, 4:4:4, sha256 e2db11d5...) is edited in the DCT
domain, with no re-encoding: jpegtran -crop takes a 464 x 64 block of the image's own background
(x 536, y 800: every coefficient there is the plain black block), -drop puts it over the Knox mark
(rows 379..430, columns 541..991 -> the 8 x 8 blocks x 536..999, y 376..439). Every other block keeps
its coefficients, so the decoded picture is unchanged outside that rectangle (checked: 0 pixels
differ), and inside it is exactly the background (0, 0, 0). SAMSUNG, Galaxy S9 and "Powered by
android" stay as they are; the markers (EXIF, Ducky, Adobe) are copied, Huffman tables re-optimised
(as Photoshop's are).

Writes OUT_DIR/logo.jpg, logo.jpg.png (the decoded pixels), up_param.img (upparam-pack.py: the stock
image with logo.jpg replaced; its unchanged-rebuild self-test must pass). Nothing is flashed.
"""
import hashlib
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
STOCK_SHA = 'e2db11d54f11ee8b82692e5f844d000d4db10ff99a9175412b632ed057c8bf97'
SRC_BLOCK = (464, 64, 536, 800)     # w, h, x, y: plain background, 8-aligned
DST = (536, 376)                    # over the Knox mark (rows 379..430, columns 541..991)


def main():
    up = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'backup/20261102-185306/UP_PARAM.img'
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / 'build/brand/upparam-native'
    out.mkdir(parents=True, exist_ok=True)
    with tarfile.open(up, mode='r:') as t:
        stock = t.extractfile('logo.jpg').read()
    if hashlib.sha256(stock).hexdigest() != STOCK_SHA:
        raise SystemExit(f'{up}: logo.jpg is not the stock splash (sha {hashlib.sha256(stock).hexdigest()[:16]})')
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        (td / 'stock.jpg').write_bytes(stock)
        w, h, x, y = SRC_BLOCK
        subprocess.run(['jpegtran', '-copy', 'none', '-crop', f'{w}x{h}+{x}+{y}', '-outfile',
                        str(td / 'bg.jpg'), str(td / 'stock.jpg')], check=True)
        subprocess.run(['jpegtran', '-copy', 'all', '-optimize', '-drop', f'+{DST[0]}+{DST[1]}',
                        str(td / 'bg.jpg'), '-outfile', str(out / 'logo.jpg'), str(td / 'stock.jpg')],
                       check=True)
        a = np.asarray(Image.open(td / 'stock.jpg').convert('RGB'))
    b_img = Image.open(out / 'logo.jpg')
    if b_img.size != (1079, 1747) or b_img.info.get('progressive'):
        raise SystemExit('unexpected output format')
    b = np.asarray(b_img.convert('RGB'))
    if a[y:y + h, x:x + w].max() != 0:
        raise SystemExit('the source block is not plain background')
    d = (a != b).any(2)
    d[DST[1]:DST[1] + h, DST[0]:DST[0] + w] = False
    if d.any() or b[DST[1]:DST[1] + h, DST[0]:DST[0] + w].max() != 0:
        raise SystemExit('the edit changed more than the Knox mark')
    Image.fromarray(b).save(out / 'logo.jpg.png')
    subprocess.run([sys.executable, '-I', str(ROOT / 'tools/brand/upparam-pack.py'), str(up),
                    str(out / 'up_param.img'), f'logo.jpg={out / "logo.jpg"}'], check=True)
    for f in ('logo.jpg', 'logo.jpg.png', 'up_param.img'):
        print(hashlib.sha256((out / f).read_bytes()).hexdigest(), out / f)


if __name__ == '__main__':
    main()
