#!/usr/bin/env python3
# sbtk-pack.py BUILD/sbtk: packs the toolkit's fonts (sbtk-fonts.py) and icons (sbtk-icons.mjs) into
# BUILD/sbtk/sbtk.pak, the format tk_pack.c reads (premultiplied 0xAARRGGBB images).
import json, os, struct, sys
import numpy as np
from PIL import Image

d = sys.argv[1]
FONTS = ['it400', 'it500', 'it600', 'it700', 'it800', 'of300']
ents, blobs = [], []
for i, n in enumerate(FONTS):
    ents.append((n, 1, 0, 0, i)); blobs.append(open(os.path.join(d, 'fonts', n + '.ttf'), 'rb').read())
    ents.append((n + '.lay', 2, 0, 0, i)); blobs.append(open(os.path.join(d, 'fonts', n + '.lay'), 'rb').read())
for m in json.load(open(os.path.join(d, 'icons', 'icons.json'))):
    im = np.asarray(Image.open(os.path.join(d, 'icons', m['name'] + '.png')).convert('RGBA')).astype(np.float64)
    a = im[..., 3:4] / 255.0
    pm = np.concatenate([np.round(im[..., :3] * a), im[..., 3:4]], axis=2).astype(np.uint8)
    # bytes B, G, R, A = little-endian 0xAARRGGBB
    bgra = pm[..., [2, 1, 0, 3]].copy()
    h, w = bgra.shape[:2]
    ents.append((m['name'], 3, w, h, int(round(m['pad'] * 256)))); blobs.append(bgra.tobytes())
off = 16 + 64 * len(ents)
off = (off + 63) & ~63
out = bytearray(b'SBTKPAK1' + struct.pack('<II', len(ents), 0))
data = bytearray()
for (name, t, w, h, aux), b in zip(ents, blobs):
    assert len(name) < 32, name
    out += name.encode().ljust(32, b'\0') + struct.pack('<IIIIIiII', t, w, h, off + len(data), len(b), aux, 0, 0)
    data += b
    data += b'\0' * ((-len(data)) % 64)
out += b'\0' * (off - len(out))
out += data
open(os.path.join(d, 'sbtk.pak'), 'wb').write(out)
print(f'sbtk.pak: {len(ents)} entries, {len(out)} bytes')
