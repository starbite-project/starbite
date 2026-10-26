#!/usr/bin/env python3
"""Advance width of a string in the bundled Outfit variable font.
    measure.py TEXT SIZE WEIGHT  -> width in px (float)
"""
import sys
from pathlib import Path
from PIL import ImageFont

s, size, weight = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
f = ImageFont.truetype(str(Path(__file__).parent / 'fonts/Outfit-wght.ttf'), int(round(size)))
f.set_variation_by_axes([weight])
print(f.getlength(s))
