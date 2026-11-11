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
           ['a62-' + k for k in ('calendar-bg', 'weather', 'clock', 'camera', 'photos', 'music', 'notes', 'calc', 'phone', 'messages', 'browser', 'settings', 'contacts', 'files', 'light', 'battery', 'terminal', 'medical', 'sos')] +
           ['sb-wifi', 'sb-batt', 'mark-star', 'star', 'chev', 'alarm', 'torch', 'torch-lit', 'flame', 'lock-c', 'lock-o', 'recovery', 'lab',
            'home-batt', 'home-star', 'search', 'arrow',
            'cc-air', 'cc-cell', 'cc-wifi', 'cc-bt', 'cc-sun', 'cc-vol', 'cc-flame', 'cc-calc', 'cc-camera',
            'kb-shift', 'kb-shift-on', 'kb-delete', 'thermometer',
            'call', 'search15', 'back', 'send', 'heart', 'star-e', 'em-flame', 'e46-heat', 'e46-power', 'e46-sos', 'e46-medical',
            'a62-heat', 'a62-power'] + [f'tab-{k}{v}' for k in ('fav', 'rec', 'con', 'pad') for v in ('', '-on')] +
           [f'cc-{k}{v}' for k in ('light', 'rot', 'save', 'timer', 'wifi2') for v in ('', '-on')])
FONTS = ['tx400', 'tx500', 'tx600', 'tx700', 'dp400', 'dp600', 'dp700', 'of300']   # sbtk.h's TK_TX400 .. TK_FOF300
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
