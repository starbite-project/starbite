#!/usr/bin/env python3
"""Contact sheet of the boot-menu host renders: each state before | after, side by side.

    contact.py SCREENS_DIR OUT.png      (SCREENS_DIR holds before/, after/, real-before/, real-after/)"""
import sys
from pathlib import Path
from PIL import Image, ImageDraw

root, out = Path(sys.argv[1]), Path(sys.argv[2])
W, H = 240, 493
pairs = []
for pre in ('', 'real-'):
    b, a = root / f'{pre}before', root / f'{pre}after'
    for p in sorted(b.glob('*.png')):
        if (a / p.name).exists():
            pairs.append((f'{pre}{p.stem}', p, a / p.name))
cols = 4                                   # 4 pairs a row
rows = (len(pairs) + cols - 1) // cols
sheet = Image.new('RGB', (cols * (2 * W + 24), rows * (H + 30)), (12, 12, 18))
d = ImageDraw.Draw(sheet)
for i, (name, b, a) in enumerate(pairs):
    x, y = (i % cols) * (2 * W + 24), (i // cols) * (H + 30)
    for k, p in enumerate((b, a)):
        sheet.paste(Image.open(p).convert('RGB').resize((W, H), Image.LANCZOS), (x + k * W, y + 24))
    d.text((x + 4, y + 6), f'{name}: before | after', fill=(220, 220, 230))
sheet.save(out)
print(out, sheet.size, len(pairs), 'pairs')
