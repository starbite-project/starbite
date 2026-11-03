#!/usr/bin/env python3
"""Compare the menu v7 assets with the concept as Chrome draws it (the approval reference).

    build/venv/bin/python tools/brand/menu7-compare.py [OUTDIR]   (default build/menu7-work/assets/compare)

Builds a reference page from the concept's own markup (tools/brand/menu7-tg.mjs: its <defs>,
<symbol>s, tile(e) SVGs, the Google Fonts link it uses) and its own drawSam/buildHeight code, has
headless Chrome render it at device scale 1, and compares it with what the container holds:

  tiles   every concept tile at 124 and 380 px (glass's composite: gradient, glyph layer, sheen,
          rim, as menu7-assets.py's preview draws it) on the same background
  icons   every SB7_I_* line icon at its size and stroke, every provider glyph with its shadow
  sam     SB7_S_SAMSUNG against the page's samCv canvas (drawSam k = 1), and the height field
          against buildHeight()'s hm canvas, both read back as exact alpha values
Writes side-by-side PNGs (reference | ours | |difference| x 4) and prints the mean and max
absolute difference per item (0..255). Needs network for fonts.googleapis.com (as the concept).
"""
import base64
import importlib.util
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
CHROME = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


A = load('m7a', ROOT / 'tools/brand/menu7-assets.py')
D = load('sb7d', ROOT / 'tools/menu7/sb7-dump.py')
BG = (20, 24, 40)


def chrome_shot(html, w, h, td):
    p = Path(td) / 'ref.html'
    p.write_text(html)
    o = Path(td) / 'ref.png'
    subprocess.run([CHROME, '--headless=new', f'--window-size={w},{h}', '--force-device-scale-factor=1',
                    '--hide-scrollbars', '--virtual-time-budget=8000', f'--screenshot={o}',
                    f'file://{p}'], check=True, capture_output=True)
    return np.asarray(Image.open(o).convert('RGB')).astype(np.float64)[:h, :w]


def chrome_dom(html, td):
    p = Path(td) / 'dom.html'
    p.write_text(html)
    r = subprocess.run([CHROME, '--headless=new', '--virtual-time-budget=8000', '--dump-dom',
                        f'file://{p}'], check=True, capture_output=True, text=True)
    m = re.search(r'<pre id="OUT">(.*?)</pre>', r.stdout, re.S)
    return json.loads(m.group(1))


def put(canvas, x, y, rgba):
    h, w = rgba.shape[:2]
    canvas[y:y + h, x:x + w] = A.over(canvas[y:y + h, x:x + w], rgba)


def side(ref, ours, path):
    d = np.abs(ref - ours)
    im = np.concatenate([ref, ours, np.clip(d * 4, 0, 255)], axis=1)
    Image.fromarray(np.clip(np.rint(im), 0, 255).astype(np.uint8)).save(path)
    return d


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'build/menu7-work/assets/compare'
    out.mkdir(parents=True, exist_ok=True)
    art = A.artwork()
    A.ART = art
    blob = (ROOT / 'build/brand/menu7.sb7').read_bytes()
    base, size, secs = D.parse(blob)
    imgs = {i: D.image(b) for (t, i), b in secs.items() if t == 2}
    tiles = {i: secs[(3, i)] for (t, i) in secs if t == 3}

    def rgba_of(iid):
        m = imgs[iid]
        if m['fmt'] == 1:
            a = np.frombuffer(m['px'], np.uint8).reshape(m['h'], m['w']).astype(np.float64)
            r = np.zeros(a.shape + (4,))
            r[..., :3] = 255
            r[..., 3] = a
            return r
        p = np.frombuffer(m['px'], np.uint8).reshape(m['h'], m['w'], 4).astype(np.float64)
        al = p[..., 3:4]
        rgb = np.where(al > 0, p[..., [2, 1, 0]] * 255 / np.maximum(al, 1), 0)
        return np.concatenate([rgb, al], axis=-1)

    head = ('<!doctype html><html><head><meta charset="utf-8">'
            '<link href="https://fonts.googleapis.com/css2?family=Inter:wght@300;400;500;600;700&'
            'family=Inter+Tight:wght@300;500;600;700;800&family=JetBrains+Mono:wght@500&display=swap" '
            'rel="stylesheet"><style>html,body{margin:0;background:rgb(%d,%d,%d);overflow:hidden}'
            '.screen{font-feature-settings:"cv11","ss01";font-family:Inter}'
            'svg.i{fill:none;stroke:currentColor;stroke-width:2;stroke-linecap:round;'
            'stroke-linejoin:round;overflow:visible;color:#fff}'
            '.p{position:absolute}.tile svg{fill:#fff;filter:drop-shadow(0 4px 10px rgba(0,0,0,.25))}'
            '</style></head><body class="screen"><svg width="0" height="0" style="position:absolute">'
            % BG + art['defs'] + '</svg>')
    report = []
    with tempfile.TemporaryDirectory() as td:
        # ---- tiles -------------------------------------------------------------------------
        concept = art['tiles']
        for si, T in ((1, 124), (3, 380)):
            cols = 6 if T == 124 else 4
            g = 24
            rows = (len(concept) + cols - 1) // cols
            W, H = cols * (T + g) + g, rows * (T + g) + g
            body = []
            ours = np.zeros((H, W, 4))
            ours[..., :3] = BG
            ours[..., 3] = 255
            for i, t in enumerate(concept):
                x, y = g + (i % cols) * (T + g), g + (i // cols) * (T + g)
                svg = t['svg'].replace('<svg class="ot"', f'<svg class="p" style="left:{x}px;top:{y}px;'
                                       f'width:{T}px;height:{T}px"').replace(f'tg-{t["id"]}', f'tg-{t["id"]}-{T}')
                body.append(svg)
                tid = i + 1          # menu7-assets numbers the concept tiles first, in order
                c1, c2, *gl, fl, pad = __import__('struct').unpack('<IIHHHHHH', tiles[tid])
                lay = rgba_of(gl[si]) if gl[si] else np.zeros((T, T, 4))
                tp = A.tile_preview('#%06x' % c1, '#%06x' % c2, lay, T)
                put(ours, x, y, tp)
            ref = chrome_shot(head + ''.join(body) + '</body></html>', W, H, td)
            d = side(ref, ours[..., :3], out / f'tiles-{T}.png')
            for i, t in enumerate(concept):
                x, y = g + (i % cols) * (T + g), g + (i // cols) * (T + g)
                c = d[y:y + T, x:x + T]
                report.append((f'tile {T} {t["id"]}', c.mean(), c.max()))
        # ---- icons and provider glyphs -------------------------------------------------------
        items = []
        for iid, (sid, sz, sw) in A.ICONS.items():
            items.append((iid, f'<svg class="i p" style="width:{sz}px;height:{sz}px;stroke-width:{sw}">'
                               f'<use href="#{sid}"/></svg>', sz, sz))
        items.append((22, '<svg class="p" style="width:150px;height:150px;color:#ffd60a">'
                          '<use href="#i-warn"/></svg>', 150, 150))
        items.append((23, '<svg class="p" style="width:84px;height:40px;color:#fff" viewBox="0 0 44 20">'
                          '<use href="#i-bat"/></svg>', 84, 40))
        for iid, g in A.PROVIDERS.items():
            if g == 'other':
                continue
            items.append((iid, f'<div class="tile p"><svg viewBox="0 0 100 100" style="width:96px;'
                               f'height:96px;display:block"><use href="#g-{g}"/></svg></div>', 96, 96))
        x, y, rowh, W = 30, 30, 0, 1400
        placed, body = [], []
        for iid, html, w, h in items:
            if x + w + 60 > W:
                x, y, rowh = 30, y + rowh + 60, 0
            body.append(html.replace('class="i p" style="', f'class="i p" style="left:{x}px;top:{y}px;')
                        .replace('class="p" style="', f'class="p" style="left:{x}px;top:{y}px;')
                        .replace('class="tile p">', f'class="tile p" style="left:{x}px;top:{y}px">'))
            placed.append((iid, x, y, w, h))
            x += w + 60
            rowh = max(rowh, h)
        H = y + rowh + 60
        ref = chrome_shot(head + ''.join(body) + '</body></html>', W, H, td)
        ours = np.zeros((H, W, 4))
        ours[..., :3] = BG
        ours[..., 3] = 255
        for iid, x, y, w, h in placed:
            m = imgs[iid]
            nx, ny, nw, nh = m['nom']
            put(ours, x - nx, y - ny, rgba_of(iid))
        d = side(ref, ours[..., :3], out / 'icons.png')
        for iid, x, y, w, h in placed:
            c = d[max(y - 20, 0):y + h + 24, max(x - 20, 0):x + w + 20]
            report.append((f'image {iid}', c.mean(), c.max()))
        # ---- SAMSUNG and the height field: the page's own canvas code ------------------------
        html = (ROOT / 'docs/concepts/ios27-boot.html').read_text()
        code = html[html.index('const SW=17;'):html.index('/* ================= layout / scaling')]
        page = ('<!doctype html><html><body><canvas id="samCv" width="1300" height="220"></canvas><script>'
                'const $=id=>document.getElementById(id);const clamp=(x,a=0,b=1)=>Math.min(b,Math.max(a,x));'
                + code + ';buildHeight();'
                'const A=c=>{const d=c.getContext("2d").getImageData(0,0,c.width,c.height).data;'
                'let s="";for(let i=3;i<d.length;i+=4)s+=String.fromCharCode(d[i]);return btoa(s)};'
                'const pre=document.createElement("pre");pre.id="OUT";'
                'pre.textContent=JSON.stringify({sam:A(samCv),hm:A(hm)});document.body.appendChild(pre);'
                '</script></body></html>')
        dom = chrome_dom(page, td)
    sam_ref = np.frombuffer(base64.b64decode(dom['sam']), np.uint8).reshape(220, 1300).astype(np.float64)
    hm_ref = np.frombuffer(base64.b64decode(dom['hm']), np.uint8).reshape(110, 650).astype(np.float64)
    sam = np.frombuffer(imgs[80]['px'], np.uint8).reshape(220, 1300).astype(np.float64)
    hb_ = secs[(5, 0)]
    hm = np.frombuffer(hb_[16:16 + 650 * 110], np.uint8).reshape(110, 650).astype(np.float64)
    for name, r, o in (('samsung', sam_ref, sam), ('height', hm_ref, hm)):
        d = side(np.repeat(r[..., None], 3, -1), np.repeat(o[..., None], 3, -1), out / f'{name}.png')
        report.append((name, d.mean(), d.max()))
    print(f'{"item":28} {"mean":>7} {"max":>5}')
    for n, mean, mx in report:
        print(f'{n:28} {mean:7.3f} {mx:5.0f}')
    print(f'written: {out}/tiles-124.png tiles-380.png icons.png samsung.png height.png')


if __name__ == '__main__':
    main()
