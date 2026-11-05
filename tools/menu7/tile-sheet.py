#!/usr/bin/env python3
"""The OS tiles of named catalog entries, as glass draws them (sb7_tile_for: exact key, then the
category's default), labelled, from a built container:

    build/venv/bin/python tools/menu7/tile-sheet.py build/brand/menu7.sb7 OUT.png [--size 220]
        [--title TEXT] [key:category:label ...]

Without entries: the menu's visible systems (ios16 ... linux-pmos, engineer).
"""
import argparse
import importlib.util
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('m7a', ROOT / 'tools/brand/menu7-assets.py')
m7a = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m7a)

VISIBLE = ['ios16:apple:iOS 16', 'mac13:apple:macOS Ventura', 'ios26:apple:iOS 26',
           'win11:microsoft:Windows 11', 'android:android:Android 17',
           'chromeos:android:ChromiumOS', 'linux-ubuntu:linux:Ubuntu',
           'linux-pmos:linux:postmarketOS', 'engineer:starbite:Engineer OS']
SIZES = {92: 0, 124: 1, 220: 2, 380: 3}


def sections(blob):
    assert blob[:8] == b'SB7ASSET'
    _, n, _, _, _, _ = struct.unpack_from('<IIIIII', blob, 8)
    out = {}
    for i in range(n):
        t, sid, off, ln = struct.unpack_from('<IIII', blob, 32 + 16 * i)
        out[(t, sid)] = blob[off:off + ln]
    return out


def tile_for(secs, key, cat):
    tm = secs[(m7a.T_TILEMAP, 0)]
    best = None
    for i in range(0, len(tm), 28):
        k = tm[i:i + 24].rstrip(b'\0').decode()
        tid, c, fl = struct.unpack_from('<HBB', tm, i + 24)
        if not k:
            if c == cat and best is None:
                best = tid
        elif k == key:
            return tid
    return best


def image(secs, iid):
    b = secs[(m7a.T_IMAGE, iid)]
    w, h, fmt, enc, _, nx, ny, nw, nh, ln = struct.unpack_from('<HHBBHHHHHI', b, 0)
    hs = struct.calcsize('<HHBBHHHHHI')
    data = b[hs:hs + ln]
    raw = m7a.unrle(data, w * h, 4) if enc == m7a.RLE else data
    px = np.frombuffer(raw, '<u4').reshape(h, w)
    a = (px >> 24 & 255).astype(np.float64)
    rgb = np.stack([(px >> s & 255) for s in (16, 8, 0)], -1).astype(np.float64)
    rgb = np.where(a[..., None] > 0, rgb * 255 / np.maximum(a[..., None], 1), 0)
    return np.concatenate([rgb, a[..., None]], -1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('sb7')
    ap.add_argument('out')
    ap.add_argument('entries', nargs='*')
    ap.add_argument('--size', type=int, default=220)
    ap.add_argument('--title', default='')
    a = ap.parse_args()
    secs = sections(Path(a.sb7).read_bytes())
    ents = [e.split(':', 2) for e in (a.entries or VISIBLE)]
    T, g, lab = a.size, 28, 44
    top = 60 if a.title else 0
    W, H = len(ents) * (T + g) + g, T + 2 * g + lab + top
    sheet = np.zeros((H, W, 4))
    sheet[..., :3] = (14, 18, 34)
    sheet[..., 3] = 255
    for i, (key, cat, _) in enumerate(ents):
        tid = tile_for(secs, key, m7a.CAT[cat])
        c1, c2, *gl, fl, _ = struct.unpack('<IIHHHHHH', secs[(m7a.T_TILE, tid)])
        gid = gl[SIZES[T]]
        lay = image(secs, gid) if gid else np.zeros((T, T, 4))
        tp = m7a.tile_preview('#%06x' % c1, '#%06x' % c2, lay, T)
        x, y = g + i * (T + g), g + top
        sheet[y:y + T, x:x + T] = m7a.over(sheet[y:y + T, x:x + T], tp)
    im = Image.fromarray(np.clip(sheet, 0, 255).astype(np.uint8)[..., :3])
    d = ImageDraw.Draw(im)
    try:
        f = ImageFont.truetype(str(ROOT / 'tools/brand/fonts/InterTight[wght].ttf'), 26)
        ft = ImageFont.truetype(str(ROOT / 'tools/brand/fonts/InterTight[wght].ttf'), 34)
    except OSError:
        f = ft = ImageFont.load_default()
    if a.title:
        d.text((g, 18), a.title, fill=(235, 235, 245), font=ft)
    for i, (key, cat, label) in enumerate(ents):
        x = g + i * (T + g) + T / 2
        d.text((x, g + top + T + 12), label, fill=(235, 235, 245), font=f, anchor='mt')
    im.save(a.out)
    print(a.out)


if __name__ == '__main__':
    sys.exit(main())
