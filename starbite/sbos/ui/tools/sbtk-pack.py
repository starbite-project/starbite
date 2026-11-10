#!/usr/bin/env python3
# sbtk-pack.py BUILD/sbtk: packs the toolkit's fonts (sbtk-fonts.py) and icons (sbtk-icons.mjs) into
# BUILD/sbtk/sbtk.pak, the format tk_pack.c reads (premultiplied 0xAARRGGBB images).
import json, os, struct, sys
import numpy as np
from PIL import Image

d = sys.argv[1]
phone = '--phone' in sys.argv
# the phone's pack: only what the shell draws (the host pack keeps every size for the tools)
SYS = ['ios16', 'mac13', 'ios26', 'win11', 'android', 'chromeos', 'linux-ubuntu', 'linux-pmos', 'starbite-os', 'engineer', 'utility']
KEEP = set(['t46-' + k for k in ('apple', 'microsoft', 'google', 'linux', 'starbite')] + ['t38-' + k for k in SYS] + ['t66-' + k for k in SYS] +
           ['t52-' + k for k in ('win11', 'ios26', 'engineer', 'ios16')] +
           ['a62-' + k for k in ('calendar', 'weather', 'clock', 'camera', 'photos', 'music', 'notes', 'calc', 'phone', 'messages', 'browser', 'settings')] +
           ['sb-wifi', 'sb-batt', 'mark-star', 'star', 'chev', 'alarm', 'torch', 'torch-lit', 'flame', 'lock-c', 'lock-o', 'recovery', 'lab',
            'home-batt', 'home-star', 'search', 'arrow'])
FONTS = ['it400', 'it500', 'it600', 'it700', 'it800', 'of300']
ents, blobs = [], []
for i, n in enumerate(FONTS):
    ents.append((n, 1, 0, 0, i)); blobs.append(open(os.path.join(d, 'fonts', n + '.ttf'), 'rb').read())
    ents.append((n + '.lay', 2, 0, 0, i)); blobs.append(open(os.path.join(d, 'fonts', n + '.lay'), 'rb').read())
for m in json.load(open(os.path.join(d, 'icons', 'icons.json'))):
    if phone and m['name'] not in KEEP:
        continue
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
name = 'sbtk-phone.pak' if phone else 'sbtk.pak'
open(os.path.join(d, name), 'wb').write(out)
print(f'{name}: {len(ents)} entries, {len(out)} bytes')
