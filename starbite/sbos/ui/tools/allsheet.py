#!/usr/bin/env python3
# allsheet.py REFDIR HOSTDIR OUT.png STATE...: every state as concept (Chrome) over sbtk (host), small, in one row
# per pair, with the mean error from compare.py's summary.
import os, sys
import numpy as np
from PIL import Image, ImageDraw
ref, host, out = sys.argv[1:4]
states = sys.argv[4:]
summ = {}
sp = os.path.join(os.path.dirname(out), 'summary.txt')
if os.path.exists(sp):
    for l in open(sp): summ[l.split()[0]] = l.split()[2]
W, H = 240, 493
cols = []
for st in states:
    a = Image.open(os.path.join(ref, st + '.png')).convert('RGB').crop((0, 0, 1440, 2960)).resize((W, H), Image.LANCZOS)
    b = Image.open(os.path.join(host, st + '.ppm')).convert('RGB').resize((W, H), Image.LANCZOS)
    c = Image.new('RGB', (W, 2 * H + 40), (12, 14, 20))
    c.paste(a, (0, 20)); c.paste(b, (0, H + 40))
    d = ImageDraw.Draw(c)
    d.text((4, 4), f'{st}  concept', fill=(230, 230, 230))
    d.text((4, H + 24), f'sbtk  mean {summ.get(st, "?")}', fill=(140, 220, 160))
    cols.append(c)
sheet = Image.new('RGB', (len(cols) * (W + 6), 2 * H + 40), (0, 0, 0))
for i, c in enumerate(cols): sheet.paste(c, (i * (W + 6), 0))
sheet.save(out)
