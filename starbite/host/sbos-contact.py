#!/usr/bin/env python3
"""Starbite OS contact sheet (docs/STARBITE-OS.md 12.2): every preview PNG in a grid with its name, one PNG.
usage: sbos-contact.py OUT.png DIR [columns] [width-per-screen]"""
import sys, os
from PIL import Image, ImageDraw, ImageFont

out, d = sys.argv[1], sys.argv[2]
cols = int(sys.argv[3]) if len(sys.argv) > 3 else 8
w = int(sys.argv[4]) if len(sys.argv) > 4 else 288
names = sorted(n for n in os.listdir(d) if n.endswith('.png') and n != os.path.basename(out))
h = w * 2960 // 1440
lab = 34
pad = 16
rows = (len(names) + cols - 1) // cols
sheet = Image.new('RGB', (pad + cols * (w + pad), pad + rows * (h + lab + pad)), (24, 24, 28))
dr = ImageDraw.Draw(sheet)
try:
    font = ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial.ttf', 22)
except OSError:
    font = ImageFont.load_default()
for i, n in enumerate(names):
    im = Image.open(os.path.join(d, n)).convert('RGB').resize((w, h), Image.LANCZOS)
    x = pad + (i % cols) * (w + pad)
    y = pad + (i // cols) * (h + lab + pad)
    sheet.paste(im, (x, y + lab))
    dr.text((x + 2, y + 4), n[:-4], fill=(220, 220, 225), font=font)
sheet.save(out)
print(f'contact sheet: {len(names)} screens, {sheet.size[0]}x{sheet.size[1]}: {out}')
