#!/usr/bin/env python3
# sheet.py FRAMES.bgra W H OUT.png FRAME...: a contact sheet of a film's raw frames (numbered).
import sys
import numpy as np
from PIL import Image, ImageDraw
path, w, h, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
fr = [int(x) for x in sys.argv[5:]]
raw = np.memmap(path, dtype=np.uint8, mode='r')
n = raw.size // (w * h * 4)
tiles = []
for i in fr:
    i = min(i, n - 1)
    a = np.array(raw[i * w * h * 4:(i + 1) * w * h * 4]).reshape(h, w, 4)[..., [2, 1, 0]]
    im = Image.fromarray(a).resize((w // 3, h // 3), Image.LANCZOS)
    ImageDraw.Draw(im).text((6, 6), str(i), fill=(255, 255, 0))
    tiles.append(im)
sheet = Image.new('RGB', (sum(t.width for t in tiles), tiles[0].height))
x = 0
for t in tiles:
    sheet.paste(t, (x, 0)); x += t.width
sheet.save(out)
