#!/usr/bin/env python3
"""S-Boot's splash for menu v7: the concept's splash at the intro's t = 0, drawn from the v7
asset container exactly as intro7's first frame draws it (docs/MENU7-PORT.md 4.3, D5), so the
hand-over from S-Boot's logo.jpg to the loader's first frame shows no change.

    build/venv/bin/python tools/brand/menu7-splash.py [build/brand/menu7.sb7]

At t = 0 (geometry docs/concepts/boot-geometry/intro-0.json): black; SAMSUNG (SB7_S_SAMSUNG,
white) at (70, 1270); "Galaxy S9" Inter Tight 500 60, +.005 em, centred, baseline 1580; "Powered by"
Inter 400 31, +.02 em, #8d8d92, centred, baseline 2518; "starbite" Inter Tight 300 66, -.01 em,
pen 614, baseline 2598; the star (SB7_I_GSTAR_66, #6fb3ff) at (828, 2542.3), 27.72 px. Text at
whole-pixel glyph origins, as glass draws at scale 1.

Writes build/brand/upparam-v7/logo.jpg: the 1079 x 1747 region S-Boot draws 1:1 at (180, 1144),
baseline JPEG, quality 95, 4:4:4 (as Samsung's own logo.jpg), and logo.jpg.png (the exact pixels)
and splash-panel.png (the whole panel). Not flashed: tools/brand/upparam-pack.py packs it later.
"""
import importlib.util
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('sb7dump', ROOT / 'tools/menu7/sb7-dump.py')
sb7 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sb7)

W, H = 1440, 2960
LOGO = (180, 1144, 1079, 1747)


def blend(canvas, x0, y0, cov, rgb, a=1.0):
    """Straight-alpha colour over the canvas through coverage cov (0..255), clipped."""
    h, w = cov.shape
    x1, y1 = max(x0, 0), max(y0, 0)
    x2, y2 = min(x0 + w, W), min(y0 + h, H)
    if x2 <= x1 or y2 <= y1:
        return
    c = cov[y1 - y0:y2 - y0, x1 - x0:x2 - x0].astype(np.float64)[..., None] / 255.0 * a
    dst = canvas[y1:y2, x1:x2]
    canvas[y1:y2, x1:x2] = dst + (np.array(rgb, np.float64) - dst) * c


def text(canvas, font, s, x, y, track_em, rgb, center=False):
    track = track_em * font.px
    if center:
        w, _ = font.width(s, track)
        x = 720 - w / 2
    pen = int(round(x * 64))
    yb = int(round(y))
    prev = None
    for ch in s:
        i = font.index[ord(ch)]
        if prev is not None:
            pen += font.kern.get((prev, i), 0)
        cp, adv, w, h, bx, by, bits = font.glyphs[i]
        if w:
            cov = np.frombuffer(bits, np.uint8).reshape(h, w)
            blend(canvas, ((pen + 32) >> 6) + bx, yb + by, cov, rgb)
        pen += adv + int(round(track * 64))
        prev = i


def image_scaled(canvas, img, x, y, w, h, rgb):
    """An A8 image's nominal box onto (x, y, w, h), bilinear (as G7_IMAGE when scaled)."""
    nx, ny, nw, nh = img['nom']
    src = np.frombuffer(img['px'], np.uint8).reshape(img['h'], img['w']).astype(np.float64)
    sx, sy = nw / w, nh / h
    X0, Y0 = int(np.floor(x - nx / sx)), int(np.floor(y - ny / sy))
    X1, Y1 = int(np.ceil(x + (img['w'] - nx) / sx)), int(np.ceil(y + (img['h'] - ny) / sy))
    gy, gx = np.mgrid[Y0:Y1, X0:X1] + .5
    u = nx + (gx - x) * sx - .5
    v = ny + (gy - y) * sy - .5
    u0, v0 = np.floor(u).astype(int), np.floor(v).astype(int)
    fu, fv = u - u0, v - v0
    pad = np.pad(src, 1)

    def at(a, b):
        return pad[np.clip(b + 1, 0, pad.shape[0] - 1), np.clip(a + 1, 0, pad.shape[1] - 1)]
    cov = (at(u0, v0) * (1 - fu) * (1 - fv) + at(u0 + 1, v0) * fu * (1 - fv) +
           at(u0, v0 + 1) * (1 - fu) * fv + at(u0 + 1, v0 + 1) * fu * fv)
    blend(canvas, X0, Y0, cov, rgb)


def main():
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'build/brand/menu7.sb7'
    base, size, secs = sb7.parse(path.read_bytes())
    font = {i: sb7.Font(b) for (t, i), b in secs.items() if t == 1}
    img = {i: sb7.image(b) for (t, i), b in secs.items() if t == 2 and i < 256}
    cv = np.zeros((H, W, 3))
    sam = img[80]
    blend(cv, 70, 1270, np.frombuffer(sam['px'], np.uint8).reshape(sam['h'], sam['w']), (255, 255, 255))
    text(cv, font[10], 'Galaxy S9', 0, 1580, .005, (255, 255, 255), center=True)
    text(cv, font[26], 'Powered by', 0, 2518, .02, (0x8d, 0x8d, 0x92), center=True)
    text(cv, font[11], 'starbite', 614, 2598, -.01, (255, 255, 255))
    image_scaled(cv, img[25], 828, 2542.3, 27.72, 27.72, (0x6f, 0xb3, 0xff))
    out = ROOT / 'build/brand/upparam-v7'
    out.mkdir(parents=True, exist_ok=True)
    panel = Image.fromarray(np.clip(np.rint(cv), 0, 255).astype(np.uint8))
    panel.save(out / 'splash-panel.png')
    x, y, w, h = LOGO
    logo = panel.crop((x, y, x + w, y + h))
    logo.save(out / 'logo.jpg.png')
    logo.save(out / 'logo.jpg', quality=95, subsampling=0, progressive=False, optimize=False)
    print(f'{out / "logo.jpg"}: {w} x {h} at ({x}, {y}), {(out / "logo.jpg").stat().st_size} bytes')


if __name__ == '__main__':
    main()
