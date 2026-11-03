#!/usr/bin/env python3
"""Compare a menu v7 render (1440 x 2960, h7) with the concept screenshot of the same state
(build/menu7/concept-shots/<state>.png: headless Chrome at 1000 x 1500, solo, device scale
.4625), at the concept's scale: h7's panel is area-filtered to 666 x 1369 and the concept's
screen rect is resampled from the screenshot. Writes side by side (concept | v7 | |diff| x 4)
and prints the mean absolute error per region (in 1/255), from the state's geometry dump.

    diff.py concept.png h7.png out.png [--state menu] [--geom docs/concepts/boot-geometry/menu.json]

The concept's screen corners (r 118, D13) and the 2 px inset ring are masked out.
Acceptance (docs/MENU7-PORT.md section 5): per region mean error < 3/255.
"""
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

S = 0.4625                      # the concept's device scale in a 1000 x 1500 solo window
OX, OY = 167.0, 65.5            # the screen rect's origin in that window
W, H = 666, 1369


def main():
    a = sys.argv[1:]
    concept, mine, out = a[0], a[1], a[2]
    state = None
    geom = None
    if '--state' in a:
        state = a[a.index('--state') + 1]
    if '--geom' in a:
        geom = a[a.index('--geom') + 1]
    root = Path(__file__).resolve().parents[3]
    if state and not geom:
        geom = root / 'docs/concepts/boot-geometry' / f'{state}.json'
    c = Image.open(concept).convert('RGB')
    c = c.transform((W, H), Image.AFFINE, (1, 0, OX, 0, 1, OY), Image.BILINEAR)
    m = Image.open(mine).convert('RGB').resize((W, H), Image.BOX)
    ca = np.asarray(c).astype(np.int16)
    ma = np.asarray(m).astype(np.int16)
    d = np.abs(ca - ma)
    # mask the concept's screen corners and its inset ring
    mask = Image.new('L', (W, H), 0)
    ImageDraw.Draw(mask).rounded_rectangle((2, 2, W - 3, H - 3), radius=118 * S, fill=255)
    mk = np.asarray(mask) > 0
    regions = [('status', 0, 0, 1440, 170)]
    if geom and Path(geom).exists():
        g = json.load(open(geom))
        cards = [o['r'] for o in g if o['p'].endswith('> .card') and o.get('a', 1) > .01]
        regions.append(('title', 0, 170, 1440, cards[0]['y'] if cards else 629))
        for i, r in enumerate(cards):
            y0, y1 = max(170, r['y']), min(2620, r['y'] + r['h'])
            if y1 > y0:
                regions.append((f'section {i}', r['x'], y0, r['x'] + r['w'], y1))
        pill = next((o['r'] for o in g if o['p'] == '#menu > #pill' and o.get('a', 1) > .01), None)
        if pill:
            regions.append(('island', pill['x'], pill['y'], pill['x'] + pill['w'], pill['y'] + pill['h']))
        ov = [o for o in g if 'glass' in o and o['p'].startswith('#over')]
        if ov:
            x0 = min(o['r']['x'] for o in ov); y0 = min(o['r']['y'] for o in ov)
            x1 = max(o['r']['x'] + o['r']['w'] for o in ov); y1 = max(o['r']['y'] + o['r']['h'] for o in ov)
            regions.append(('overlay', x0, y0, x1, y1))
    regions.append(('dock', 0, 2640, 1440, 2880))
    regions.append(('whole', 0, 0, 1440, 2960))
    print(f'{"region":<12} {"mean |d|":>9} {"p99":>6} {"max":>5}   (1/255, at the concept scale)')
    worst = 0
    for name, x0, y0, x1, y1 in regions:
        X0, Y0 = int(max(0, x0 * S)), int(max(0, y0 * S))
        X1, Y1 = int(min(W, x1 * S)), int(min(H, y1 * S))
        if X1 <= X0 or Y1 <= Y0:
            continue
        sub = d[Y0:Y1, X0:X1][mk[Y0:Y1, X0:X1]]
        if not sub.size:
            continue
        mean = float(sub.mean())
        worst = max(worst, mean) if name != 'whole' else worst
        print(f'{name:<12} {mean:9.2f} {float(np.percentile(sub, 99)):6.0f} {int(sub.max()):5d}')
    heat = np.clip(d.max(axis=2) * 4, 0, 255).astype(np.uint8)
    heat[~mk] = 0
    side = Image.new('RGB', (3 * W + 16, H), (40, 40, 48))
    side.paste(c, (0, 0))
    side.paste(m, (W + 8, 0))
    side.paste(Image.fromarray(heat).convert('RGB'), (2 * W + 16, 0))
    side.save(out)
    print(f'worst region mean {worst:.2f}/255 -> {out}')


main()
