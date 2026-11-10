#!/usr/bin/env python3
# compare.py REFDIR HOSTDIR OUTDIR STATE...: the concept (headless Chrome, concept-ref.mjs) against sbtk-host's
# frame of the same state, both 1440 x 2960. Per state: OUTDIR/<state>.png (concept | sbtk | |diff| x 4) and one
# line: mean |error| per channel (0..255) over the panel and per region, and the share of pixels off by > 16 and
# > 64. The regions are the lock's parts in pt (x 3.6923).
import sys, os
import numpy as np
from PIL import Image

S = 1440 / 390
REG = {'status': (0, 0, 390, 48), 'mark+date': (0, 44, 390, 104), 'clock': (0, 96, 390, 210), 'widgets': (20, 210, 370, 300),
       'list': (0, 310, 390, 684), 'buttons': (30, 684, 360, 802)}
ref, host, out = sys.argv[1:4]
os.makedirs(out, exist_ok=True)
rows = []
for st in sys.argv[4:]:
    a = np.asarray(Image.open(os.path.join(ref, st + '.png')).convert('RGB')).astype(np.int16)[:2960]
    hp = os.path.join(host, st + '.ppm')
    b = np.asarray(Image.open(hp).convert('RGB')).astype(np.int16)
    d = np.abs(a - b)
    m = d.max(2)
    reg = ' '.join(f'{k} {d[int(y0*S):int(y1*S), int(x0*S):int(x1*S)].mean():.2f}' for k, (x0, y0, x1, y1) in REG.items())
    line = f'{st:16s} mean {d.mean():.2f}  >16 {100*(m>16).mean():.2f}%  >64 {100*(m>64).mean():.3f}%  | {reg}'
    print(line)
    rows.append(line)
    sheet = np.concatenate([a, b, np.clip(d * 4, 0, 255)], axis=1).astype(np.uint8)
    Image.fromarray(sheet).resize((sheet.shape[1] // 3, sheet.shape[0] // 3), Image.LANCZOS).save(os.path.join(out, st + '.png'))
open(os.path.join(out, 'summary.txt'), 'w').write('\n'.join(rows) + '\n')
