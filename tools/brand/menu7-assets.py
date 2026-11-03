#!/usr/bin/env python3
"""Boot menu v7 assets: the "SB7ASSET" container (shim/starlte/sb7.h, docs/MENU7-PORT.md 4.4).

    build/venv/bin/python tools/brand/menu7-assets.py [--fonts-only] [--preview DIR] [--concept-art]

Writes
    build/brand/menu7.sb7        the container (fonts, icons, provider glyphs, OS tiles and their
                                 glyph layers, the tile map, SAMSUNG, its height field, INFO)
    build/brand/menu-v7.sbm      the BOOT ramdisk: build/brand/menu-em.sbm, zero padding to 4 KiB,
                                 then menu7.sb7
    build/brand/upparam-v7/logo.jpg   S-Boot's splash = the concept's splash at the intro's t = 0
                                 (tools/brand/menu7-splash.py; not flashed)
    --preview DIR: glyph sheets, icons, tiles (composited as glass will), SAMSUNG, height field
    --concept-art: the concept's own tile and provider-glyph artwork instead of the real logos,
                 into build/brand/concept/ (menu7.sb7, menu-v7.sbm; no splash), for measuring
                 concept fidelity (compare-all.sh with CMP_RD=build/brand/concept/menu-v7.sbm)

Needs build/venv with freetype-py, uharfbuzz, fonttools, numpy, Pillow; rsvg-convert; node.
Sources of truth: the concept (docs/concepts/ios27-boot.html: its <symbol>s, TG(), the provider
and tile data, drawSam/buildHeight), extracted by tools/brand/menu7-tg.mjs, never retyped; the
text metrics of docs/concepts/boot-geometry/*.json (tools/menu7/sb7-dump.py --check-geometry).

Fonts: as the concept was rendered (and measured) by Chrome from fonts.googleapis.com. Google
serves Inter with opsz pinned at its default (14) and without the cv11 / ss01 alternates, so the
approved screens show the default 'a' and digits; every measured width (`tw`) matches that and
no other combination (see SOURCES-menu7.md). The countdown digit keeps "tnum".
"""
import argparse
import hashlib
import json
import re
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

import freetype
import numpy as np
import uharfbuzz as hb
from fontTools.ttLib import TTFont
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import menu7_logos  # noqa: E402  the real OS and company logos (tools/brand/logos/SOURCES.md)

ROOT = Path(__file__).resolve().parents[2]
FONTS = ROOT / 'tools/brand/fonts'
OUT = ROOT / 'build/brand'
CONCEPT_ART = False   # --concept-art
TOOL_VERSION = 'menu7-assets 2 (real logos)'

# ---- sb7.h constants -----------------------------------------------------------------------
T_FONT, T_IMAGE, T_TILE, T_TILEMAP, T_HEIGHT, T_INFO = 1, 2, 3, 4, 5, 6
FAM_IT, FAM_IN, FAM_JB = 1, 2, 3
FF_TNUM, FF_SUBSET = 1, 2
A8, ARGB = 1, 2
RAW, RLE = 0, 1
TF_DARK = 1
TM_PREFIX = 1
G_FIRST = 256
TILE_SIZES = (92, 124, 220, 380)
CAT = {'apple': 0, 'microsoft': 1, 'android': 2, 'linux': 3, 'starbite': 4, 'other': 5, 'lab': 6}

FONT_FILES = {
    FAM_IT: 'InterTight[wght].ttf',
    FAM_IN: 'Inter[opsz,wght].ttf',
    FAM_JB: 'JetBrainsMono[wght].ttf',
}
FONT_URLS = {
    FAM_IT: 'https://github.com/google/fonts/raw/main/ofl/intertight/InterTight%5Bwght%5D.ttf',
    FAM_IN: 'https://github.com/google/fonts/raw/main/ofl/inter/Inter%5Bopsz%2Cwght%5D.ttf',
    FAM_JB: 'https://github.com/google/fonts/raw/main/ofl/jetbrainsmono/JetBrainsMono%5Bwght%5D.ttf',
}
INTER_OPSZ = 14          # what fonts.googleapis.com serves for "Inter:wght@..." (measured, above)

TEXT = ([chr(c) for c in range(0x20, 0x7f)] +
        list('\u00a0\u00b0\u00b1\u00b5\u00b7\u00d7\u2013\u2014\u2018\u2019\u201c\u201d\u2026'
             '\u2190\u2191\u2192\u2193\u2248\u25a0\u25b2\u25bc\u2713'))
AZ = [chr(c) for c in range(0x41, 0x5b)] + [chr(c) for c in range(0x61, 0x7b)] + [' ']


def chars(s):
    return sorted(set(s) | {' '})


# id: family, weight, px, charset, flags (sb7.h's table)
FONT_SPEC = {
    1: (FAM_IT, 700, 142, chars('Choose a System'), FF_SUBSET),
    2: (FAM_IT, 700, 128, AZ, FF_SUBSET),
    3: (FAM_IT, 700, 100, TEXT, 0),
    4: (FAM_IT, 700, 92, TEXT, 0),
    5: (FAM_IT, 700, 70, TEXT, 0),
    6: (FAM_IT, 700, 40, chars('0123456789'), FF_SUBSET | FF_TNUM),
    7: (FAM_IT, 600, 66, TEXT, 0),
    8: (FAM_IT, 600, 54, TEXT, 0),
    9: (FAM_IT, 600, 46, TEXT, 0),
    10: (FAM_IT, 500, 60, chars('Galaxy S9'), FF_SUBSET),
    11: (FAM_IT, 300, 66, chars('starbite'), FF_SUBSET),
    12: (FAM_IT, 300, 56, chars('starbite'), FF_SUBSET),
    13: (FAM_IN, 500, 58, TEXT, 0),
    14: (FAM_IN, 600, 58, TEXT, 0),
    15: (FAM_IN, 600, 56, TEXT, 0),
    16: (FAM_IN, 600, 46, TEXT, 0),
    17: (FAM_IN, 600, 33, TEXT, 0),
    18: (FAM_IN, 400, 50, TEXT, 0),
    19: (FAM_IN, 400, 48, TEXT, 0),
    20: (FAM_IN, 400, 46, TEXT, 0),
    21: (FAM_IN, 400, 44, TEXT, 0),
    22: (FAM_IN, 400, 42, TEXT, 0),
    23: (FAM_IN, 400, 40, TEXT, 0),
    24: (FAM_IN, 400, 38, TEXT, 0),
    25: (FAM_IN, 400, 36, TEXT, 0),
    26: (FAM_IN, 400, 31, chars('Powered by'), FF_SUBSET),
    27: (FAM_JB, 500, 40, TEXT, 0),
    28: (FAM_JB, 500, 36, TEXT, 0),
    29: (FAM_JB, 500, 34, TEXT, 0),
}

# ---- fonts ---------------------------------------------------------------------------------

_blobs = {}


def hb_font(fam, wght, scale):
    if fam not in _blobs:
        _blobs[fam] = hb.Blob.from_file_path(str(FONTS / FONT_FILES[fam]))
    face = hb.Face(_blobs[fam])
    f = hb.Font(face)
    v = {'wght': wght}
    if fam == FAM_IN:
        v['opsz'] = INTER_OPSZ
    f.set_variations(v)
    f.scale = (scale, scale)
    return f, face.upem


def ft_face(fam, wght):
    face = freetype.Face(str(FONTS / FONT_FILES[fam]))
    tags = [a.tag for a in face.get_variation_info().axes]
    face.set_var_design_coords([INTER_OPSZ if t == 'opsz' else wght for t in tags])
    return face


def shape(f, text, feats):
    buf = hb.Buffer()
    buf.add_str(text)
    buf.guess_segment_properties()
    hb.shape(f, buf, feats)
    return buf.glyph_infos, buf.glyph_positions


LOAD = freetype.FT_LOAD_NO_HINTING | freetype.FT_LOAD_RENDER | freetype.FT_LOAD_NO_BITMAP


def ft_glyph(face, gid, px, dx=0.0, dy=0.0):
    """Coverage of glyph gid at px (26.6), the pen at (dx, dy) px inside the pixel grid.
    Returns (A8 array, left, top) with top measured upwards from the baseline (FreeType's)."""
    face.set_char_size(int(round(px * 64)))
    face.set_transform(freetype.Matrix(0x10000, 0, 0, 0x10000),
                       freetype.Vector(int(round(dx * 64)), int(round(-dy * 64))))
    face.load_glyph(gid, LOAD)
    g = face.glyph
    bm = g.bitmap
    w, h, pitch = bm.width, bm.rows, bm.pitch
    if w == 0 or h == 0:
        return np.zeros((0, 0), np.uint8), 0, 0
    a = np.frombuffer(bytes(bm.buffer), np.uint8).reshape(h, pitch)[:, :w].copy()
    return a, g.bitmap_left, g.bitmap_top


def build_font(fid):
    fam, wght, px, cs, flags = FONT_SPEC[fid]
    S = 2048 * 16
    f, upem = hb_font(fam, wght, S)
    k = px / S                          # font scale units -> px
    feats = {'kern': True}
    if flags & FF_TNUM:
        feats['tnum'] = True
    face = ft_face(fam, wght)
    glyphs, missing = [], []
    for ch in sorted(set(cs), key=ord):
        infos, pos = shape(f, ch, feats)
        if len(infos) != 1 or infos[0].codepoint == 0:
            missing.append(ch)
            continue
        gid = infos[0].codepoint
        adv = pos[0].x_advance * k
        glyphs.append((ord(ch), gid, adv))
    # kerning: shape every pair; keep pairs whose glyphs are unchanged (no calt substitution)
    kern = []
    for i, (cpa, ga, aa) in enumerate(glyphs):
        for j, (cpb, gb, ab) in enumerate(glyphs):
            infos, pos = shape(f, chr(cpa) + chr(cpb), feats)
            if len(infos) != 2 or infos[0].codepoint != ga or infos[1].codepoint != gb:
                continue
            adj = (pos[0].x_advance + pos[1].x_advance) * k - aa - ab
            a64 = int(round(adj * 64))
            if a64:
                kern.append((i, j, a64))
    # bitmaps
    bits = bytearray()
    recs = []
    for cp, gid, adv in glyphs:
        a, left, top = ft_glyph(face, gid, px)
        off = len(bits)
        h, w = a.shape
        if w and h and a.any():
            bits += a.tobytes()
        else:
            w = h = 0
            left = top = 0
        recs.append(struct.pack('<IiHHhhI', cp, int(round(adv * 64)), w, h, left, -top, off))
    # metrics (hhea)
    tt = TTFont(str(FONTS / FONT_FILES[fam]))
    hh = tt['hhea']
    m = px / tt['head'].unitsPerEm
    asc64, desc64, gap64 = (int(round(hh.ascent * m * 64)), int(round(-hh.descent * m * 64)),
                            int(round(hh.lineGap * m * 64)))
    HDR = 40
    glyph_off = HDR
    kern_off = glyph_off + 20 * len(recs)
    bits_off = align(kern_off + 8 * len(kern), 16)
    body = bytearray(struct.pack('<HHHHiiiHHIIII', px, wght, fam, len(recs), asc64, desc64, gap64,
                                 len(kern), flags, glyph_off, kern_off, bits_off, len(bits)))
    assert len(body) == HDR
    for r in recs:
        body += r
    for a, b, v in kern:
        body += struct.pack('<HHi', a, b, v)
    body += b'\0' * (bits_off - len(body))
    body += bits
    return bytes(body), {'missing': ''.join(missing), 'glyphs': len(recs), 'kern': len(kern),
                         'bytes': len(body)}


def align(n, a):
    return (n + a - 1) // a * a


# ---- RLE -----------------------------------------------------------------------------------

def rle(pixels, bpp):
    """sb7.h RLE: u16 c < 0x8000: c literal pixels; c >= 0x8000: one pixel repeated c - 0x8000
    times; each group padded to 2 bytes. pixels: 1-D numpy array (uint8 or uint32)."""
    n = len(pixels)
    out = bytearray()
    if n == 0:
        return bytes(out)
    change = np.flatnonzero(pixels[1:] != pixels[:-1]) + 1
    starts = np.concatenate(([0], change))
    lens = np.diff(np.concatenate((starts, [n])))
    minrun = 3 if bpp == 1 else 2
    raw = pixels.tobytes()
    lit_from = None

    def flush_lit(a, b):
        while a < b:
            c = min(b - a, 0x7fff)
            out.extend(struct.pack('<H', c))
            out.extend(raw[a * bpp:(a + c) * bpp])
            if (c * bpp) & 1:
                out.append(0)
            a += c

    for s, ln in zip(starts.tolist(), lens.tolist()):
        if ln >= minrun:
            if lit_from is not None:
                flush_lit(lit_from, s)
                lit_from = None
            r = ln
            while r > 0:
                c = min(r, 0x7fff)
                out.extend(struct.pack('<H', 0x8000 + c))
                out.extend(raw[s * bpp:(s + 1) * bpp])
                if bpp & 1:
                    out.append(0)
                r -= c
        elif lit_from is None:
            lit_from = s
    if lit_from is not None:
        flush_lit(lit_from, n)
    return bytes(out)


def unrle(data, count, bpp):
    out = bytearray()
    i = 0
    while len(out) < count * bpp:
        c, = struct.unpack_from('<H', data, i)
        i += 2
        if c >= 0x8000:
            px = data[i:i + bpp]
            out += px * (c - 0x8000)
            i += bpp + (bpp & 1)
        else:
            out += data[i:i + c * bpp]
            i += c * bpp + ((c * bpp) & 1)
    return bytes(out)


def image_section(arr, fmt, nominal, enc=RLE):
    """arr: HxW uint8 (A8) or HxWx4 float/uint8 RGBA straight (ARGB): stored premultiplied
    0xAARRGGBB. nominal: (nx, ny, nw, nh)."""
    if fmt == A8:
        h, w = arr.shape
        px = arr.astype(np.uint8).reshape(-1)
        bpp = 1
    else:
        h, w = arr.shape[:2]
        rgba = arr.astype(np.float64)
        # alpha rounded (not truncated) first, and the colour premultiplied by that stored alpha
        # and kept <= it: a colour channel above its alpha overflows the blend in glass
        a = np.clip(np.rint(rgba[..., 3]), 0, 255).astype(np.uint32)
        al = (a / 255.0)[..., None]
        pm = np.minimum(np.clip(np.rint(np.clip(rgba[..., :3], 0, 255) * al), 0, 255).astype(np.uint32),
                        a[..., None])
        px = ((a << 24) | (pm[..., 0] << 16) | (pm[..., 1] << 8) | pm[..., 2]).reshape(-1)
        px = px.astype('<u4')
        bpp = 4
    data = rle(px, bpp) if enc == RLE else px.tobytes()
    if enc == RLE and len(data) >= len(px.tobytes()):
        enc, data = RAW, px.tobytes()
    nx, ny, nw, nh = nominal
    return struct.pack('<HHBBHHHHHI', w, h, fmt, enc, 0, nx, ny, nw, nh, len(data)) + data


# ---- SVG rendering (rsvg-convert) ----------------------------------------------------------

def rsvg(svg, w, h):
    with tempfile.TemporaryDirectory() as td:
        p = Path(td) / 'a.svg'
        o = Path(td) / 'a.png'
        p.write_text(svg)
        subprocess.run(['rsvg-convert', '-w', str(w), '-h', str(h), str(p), '-o', str(o)],
                       check=True)
        im = Image.open(o).convert('RGBA')
        assert im.size == (w, h), (im.size, w, h)
        return np.asarray(im).astype(np.float64)


ART = None


def artwork(requests=()):
    r = subprocess.run(['node', str(ROOT / 'tools/brand/menu7-tg.mjs')],
                       input=json.dumps(list(requests)), capture_output=True, text=True,
                       check=True)
    return json.loads(r.stdout)


def svg_doc(w, h, body, defs):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
            f'width="{w}" height="{h}" viewBox="0 0 {w} {h}">{defs}{body}</svg>')


def symbol_body(sid):
    s = ART['symbols'][sid]
    return s['viewBox'], s['body']


def icon_a8(sid, size, sw, wbox=None, vb_override=None):
    """A line icon as svg.i draws it: 1 em box, fill none, stroke currentColor (white) with
    width sw in symbol units, round caps and joins. Returns (A8, nominal)."""
    vb, body = symbol_body(sid)
    nw = int(round(size)) if wbox is None else wbox[0]
    nh = int(round(size)) if wbox is None else wbox[1]
    spill = sw * max(nw, nh) / 24 / 2
    m = int(np.ceil(spill + 2))
    W, H = nw + 2 * m, nh + 2 * m
    inner = (f'<svg x="{m}" y="{m}" width="{nw}" height="{nh}" viewBox="{vb}" overflow="visible" '
             f'color="#fff" fill="none" stroke="#fff" stroke-width="{sw}" stroke-linecap="round" '
             f'stroke-linejoin="round">{body}</svg>')
    rgba = rsvg(svg_doc(W, H, inner, '<defs/>'), W, H)
    a = np.clip(np.rint(rgba[..., 3]), 0, 255).astype(np.uint8)
    return a, (m, m, nw, nh)


def glyph_a8(sid, size_px, nominal_px):
    """A filled 100-unit glyph (g-star) at nominal_px (whole pixels, the item scales it)."""
    vb, body = symbol_body(sid)
    m = 2
    W = nominal_px + 2 * m
    inner = (f'<svg x="{m}" y="{m}" width="{nominal_px}" height="{nominal_px}" viewBox="{vb}" '
             f'overflow="visible" fill="#fff">{body}</svg>')
    rgba = rsvg(svg_doc(W, W, inner, ART['defs_only']), W, W)
    return np.clip(np.rint(rgba[..., 3]), 0, 255).astype(np.uint8), (m, m, nominal_px, nominal_px)


def gauss_blur(a, sigma):
    r = int(np.ceil(3 * sigma))
    x = np.arange(-r, r + 1)
    k = np.exp(-x * x / (2 * sigma * sigma))
    k /= k.sum()
    pad = np.pad(a, r, mode='constant')
    t = np.apply_along_axis(lambda v: np.convolve(v, k, mode='valid'), 1, pad)
    return np.apply_along_axis(lambda v: np.convolve(v, k, mode='valid'), 0, t)


def over(dst, src):
    """Straight-alpha RGBA float arrays (0..255): src over dst."""
    sa = src[..., 3:4] / 255.0
    da = dst[..., 3:4] / 255.0
    oa = sa + da * (1 - sa)
    rgb = np.where(oa > 0, (src[..., :3] * sa + dst[..., :3] * da * (1 - sa)) / np.maximum(oa, 1e-9), 0)
    return np.concatenate([rgb, oa * 255.0], axis=-1)


# Provider glyphs that are the company's real logo (menu7_logos), in its own colours (Apple's
# logo is monochrome: white, as on the concept's glass): (logo, fit in the 96 px box, dx, dy,
# colour or None).
PROVIDER_LOGOS = {
    'apple': ('apple', .90, 0, -.02, '#ffffff'),
    'win': ('microsoft', .82, 0, 0, None),       # SB7_P_MICROSOFT: the Microsoft logo
    'andr': ('google-g', .86, 0, 0, None),       # SB7_P_GOOGLE: the Google "G"
    'tux': ('tux', .98, 0, 0, None),             # SB7_P_LINUX: Tux
}


def provider_glyph(gid):
    """.tile svg: 96 px, viewBox 100, fill #fff, drop-shadow(0 4px 10px rgba(0,0,0,.25)) baked.
    Apple, Microsoft, Google and Linux are the real logos (PROVIDER_LOGOS)."""
    S, m = 96, 20
    W = S + 2 * m
    if gid in PROVIDER_LOGOS and not CONCEPT_ART:
        name, fit, dx, dy, col = PROVIDER_LOGOS[gid]
        g = menu7_logos.render(name, W, W, S, fit, dx, dy, col)
    else:
        if gid == 'other':
            body = ('<circle cx="50" cy="50" r="38" fill="none" stroke="#fff" stroke-width="7"/>'
                    '<circle cx="31" cy="50" r="6.5"/><circle cx="50" cy="50" r="6.5"/>'
                    '<circle cx="69" cy="50" r="6.5"/>')
            vb = '0 0 100 100'
        else:
            vb, body = symbol_body('g-' + gid)
        inner = (f'<svg x="{m}" y="{m}" width="{S}" height="{S}" viewBox="{vb}" overflow="visible" '
                 f'fill="#fff" color="#fff">{body}</svg>')
        g = rsvg(svg_doc(W, W, inner, ART['defs_only']), W, W)
    al = g[..., 3]
    sh = gauss_blur(al, 5.0) * 0.25
    sh = np.roll(sh, 4, axis=0)
    shadow = np.zeros_like(g)
    shadow[..., 3] = sh
    return over(shadow, g), (m, m, S, S)


# ---- text into a layer (tile glyphs, the splash) -------------------------------------------

def text_layer(layer, text, fam, wght, px, ls, color, x_mid=None, x0=None, baseline=0.0):
    """Draw text the way Chrome lays it out (HarfBuzz advances, kerning, letter-spacing after
    every character, subpixel pen) into an RGBA float layer. x_mid: text-anchor middle."""
    S = 2048 * 16
    f, upem = hb_font(fam, wght, S)
    k = px / S
    infos, pos = shape(f, text, {'kern': True})
    W = sum(p.x_advance for p in pos) * k + ls * len(text)
    pen = (x_mid - W / 2) if x_mid is not None else x0
    face = ft_face(fam, wght)
    r, g, b = color
    for inf, p in zip(infos, pos):
        gx = pen + p.x_offset * k
        ix = int(np.floor(gx))
        iy = int(np.floor(baseline))
        a, left, top = ft_glyph(face, inf.codepoint, px, gx - ix, baseline - iy)
        pen += p.x_advance * k + ls
        if a.size == 0:
            continue
        h, w = a.shape
        X, Y = ix + left, iy - top
        x1, y1 = max(X, 0), max(Y, 0)
        x2, y2 = min(X + w, layer.shape[1]), min(Y + h, layer.shape[0])
        if x2 <= x1 or y2 <= y1:
            continue
        src = np.zeros((y2 - y1, x2 - x1, 4))
        src[..., 0], src[..., 1], src[..., 2] = r, g, b
        src[..., 3] = a[y1 - Y:y2 - Y, x1 - X:x2 - X]
        layer[y1:y2, x1:x2] = over(layer[y1:y2, x1:x2], src)
    return W


def x_height(fam, wght):
    f, upem = hb_font(fam, wght, 2048)       # scale 2048 per em, whatever the font's upem
    return f.get_glyph_extents(f.get_nominal_glyph(ord('x'))).y_bearing / 2048


def hexrgb(c):
    c = c.strip()
    if c.startswith('rgba') or c.startswith('rgb'):
        v = [float(x) for x in re.findall(r'[\d.]+', c)]
        return tuple(v[:3])
    c = c.lstrip('#')
    if len(c) == 3:
        c = ''.join(ch * 2 for ch in c)
    return tuple(int(c[i:i + 2], 16) for i in (0, 2, 4))


TEXT_RE = re.compile(r'<text x="([\d.]+)" y="([\d.]+)" text-anchor="middle" dominant-baseline="middle" '
                     r'font-family="([^"]+)" font-weight="(\d+)" font-size="([\d.]+)" fill="([^"]+)" '
                     r'letter-spacing="([-\d.]+)">([^<]*)</text>')


def tile_layer(tg_svg, T):
    """The glyph layer of a tile at T px: everything TG() draws (in its 100-unit box), coloured.
    Shapes by rsvg; <text> (TX) by FreeType + HarfBuzz as Chrome lays it out: anchor middle on
    the advance (letter-spacing included), dominant-baseline middle = y + x-height / 2."""
    texts = TEXT_RE.findall(tg_svg)
    shapes = TEXT_RE.sub('', tg_svg)
    assert '<text' not in shapes, tg_svg
    body = f'<svg width="{T}" height="{T}" viewBox="0 0 100 100" overflow="hidden">{shapes}</svg>'
    layer = rsvg(svg_doc(T, T, body, ART['defs']), T, T)
    k = T / 100
    for x, y, ff, w, sz, fill, ls, s in texts:
        fam = FAM_JB if 'JetBrains' in ff else FAM_IT
        w, sz, ls = int(w), float(sz), float(ls)
        s = s.replace('&gt;', '>').replace('&lt;', '<').replace('&amp;', '&')
        base = (float(y) + x_height(fam, w) * sz / 2) * k
        text_layer(layer, s, fam, w, sz * k, ls * k, hexrgb(fill), x_mid=float(x) * k, baseline=base)
    return layer


# ---- tiles ---------------------------------------------------------------------------------

# Glyph kinds TG() has no case for: a provider glyph at 58 in the 100 box, as TG's 'andr'.
EXTRA_G = {
    'g:apple': '<use href="#g-apple" x="21" y="21" width="58" height="58" fill="#fff"/>',
    'g:tux': '<use href="#g-tux" x="21" y="21" width="58" height="58" fill="#fff"/>',
    'g:flask': '<use href="#g-flask" x="21" y="21" width="58" height="58" fill="#fff"/>',
    'g:other': ('<circle cx="50" cy="50" r="25" fill="none" stroke="#fff" stroke-width="5"/>'
                '<circle cx="38" cy="50" r="4.2" fill="#fff"/><circle cx="50" cy="50" r="4.2" '
                'fill="#fff"/><circle cx="62" cy="50" r="4.2" fill="#fff"/>'),
}


# Glyph kinds 'L:<id>': a real OS logo (menu7_logos) on the tile, in the logo's own colours:
# id: (logo, fit in the tile box, dx, dy, colour or None). The tile colours are chosen per logo
# (tile_table): the brand's own background where the logo is white on it (Ubuntu orange, Kali
# blue), a light tile under a logo whose colour is the brand's blue, green or red (Windows 11,
# postmarketOS, Debian), the brand's dark under Android's green, Arch's blue and Fedora's icon.
LOGO_G = {
    'L:apple': ('apple', .54, 0, -.015, '#ffffff'),
    'L:win11': ('windows11', .50, 0, 0, None),
    'L:android': ('android', .64, 0, .01, None),
    'L:ubuntu': ('ubuntu', .60, 0, 0, None),
    'L:arch': ('arch', .62, 0, -.01, None),
    'L:pmos': ('pmos', .60, 0, -.01, None),
    'L:kali': ('kali', .64, .01, .02, None),
    'L:debian': ('debian', .58, 0, 0, None),
    'L:fedora': ('fedora', .60, 0, 0, None),
    'L:tux': ('tux', .66, 0, 0, None),
}


def logo_layer(g, T):
    name, fit, dx, dy, col = LOGO_G[g]
    return menu7_logos.render(name, T, T, T, fit, dx, dy, col)


def darker(c, f=.42):
    r, g, b = hexrgb(c)
    return '#%02x%02x%02x' % (int(r * f), int(g * f), int(b * f))


def tile_table(art):
    """(name, c1, c2, glyph, dark) per tile id, and the map entries (key, tile, category, flags)."""
    tiles = []          # index + 1 = tile id
    tmap = []
    byid = {}

    def add(name, c1, c2, g, dark=False):
        tiles.append((name, c1, c2, g, dark))
        return len(tiles)

    for t in art['tiles']:                      # the concept's demo tiles, keyed by concept id
        byid[t['id']] = add('concept:' + t['id'], t['c1'], t['c2'], t['g'], t['dark'])
    # the real OS logos (LOGO_G); c1 -> c2 as the concept's tiles, or the brand's own colours
    logo = {
        'ios26': add('ios26', '#0b2560', '#3d8cff', 'L:apple'),
        'win11': add('win11', '#ffffff', '#dbe4f0', 'L:win11', True),
        'android': add('android', '#05202c', '#0b4a5e', 'L:android'),          # #073042: Android navy
        'ubuntu': add('linux-ubuntu', '#c13a0d', '#f47b3c', 'L:ubuntu'),
        'gnome': add('linux-ubuntu-gnome', '#5e1b58', '#d2491f', 'L:ubuntu'),
        'lomiri': add('linux-ubuntu-lomiri', '#77216f', '#e95420', 'L:ubuntu'),
        'arch': add('linux-arch', '#0c1015', '#2a333e', 'L:arch'),
        'pmos': add('linux-pmos', '#ffffff', '#dfeadf', 'L:pmos', True),
        'kali': add('linux-kali', '#0a1b3c', '#367bf0', 'L:kali'),
        'debian': add('linux-debian', '#ffffff', '#eee1e6', 'L:debian', True),
        'fedora': add('linux-fedora', '#0b1a30', '#294172', 'L:fedora'),        # #294172: Fedora blue
    }
    new = {
        'lab': add('@lab', '#3a145e', '#ad61f2', 'g:flask'),
    }
    prov = {p['id']: p for p in art['providers']}
    cat_default = {
        'apple': add('default:apple', darker(prov['apple']['tc']), prov['apple']['tc'], 'L:apple'),
        'microsoft': add('default:microsoft', '#ffffff', '#dbe4f0', 'L:win11', True),
        'android': add('default:android', '#05202c', '#0b4a5e', 'L:android'),
        'linux': add('default:linux', '#15161a', '#3a3d45', 'L:tux'),
        'starbite': add('default:starbite', '#030918', '#0f2f73', 'star'),
        'other': add('default:other', darker('#8e8e93'), '#8e8e93', 'g:other'),
        'lab': add('default:lab', darker(prov['lab']['tc']), prov['lab']['tc'], 'g:flask'),
    }
    # today's catalog (docs/MENU7-PORT.md 4.4)
    real = [
        ('ios26', logo['ios26']), ('win11', logo['win11']), ('android', logo['android']),
        ('linux-ubuntu', logo['ubuntu']), ('linux', logo['ubuntu']),
        ('linux-ubuntu-gnome', logo['gnome']), ('linux-ubuntu-lomiri', logo['lomiri']),
        ('linux-arch', logo['arch']), ('linux-pmos', logo['pmos']), ('linux-kali', logo['kali']),
        ('linux-debian', logo['debian']), ('linux-fedora', logo['fedora']),
        ('linux-shell', byid['uefi']), ('linux-olddtb', byid['uefi']),
        ('linux-bringup', byid['uefi']), ('@lab', new['lab']),
    ]
    if CONCEPT_ART:   # the concept's artwork wherever the key is a concept id (ios26, win11)
        real = [(k, byid.get(k, tid)) for k, tid in real]
    for key, tid in real:
        tmap.append((key, tid, 0, 0))
    tmap.append(('lab-', byid['display'], CAT['lab'], TM_PREFIX))
    # the concept's demo entries by their concept id (tools/menu7/preview/concept-catalog.toml):
    # the ones that are a real OS above show its real logo too
    same = {'a17': logo['android'], 'phosh': logo['ubuntu'], 'gnome': logo['gnome'],
            'arch': logo['arch'], 'pmos': logo['pmos'], 'kali': logo['kali']}
    if CONCEPT_ART:
        same = {}
    for t in art['tiles']:
        if t['id'] not in dict(real):
            tmap.append((t['id'], same.get(t['id'], byid[t['id']]), 0, 0))
    for cat, tid in cat_default.items():
        tmap.append(('', tid, CAT[cat], 0))
    keys = [k for k, *_ in tmap if k]
    assert len(keys) == len(set(keys)), 'duplicate tile map key'
    # only the tiles the map reaches (the concept's own artwork for a real OS is replaced above)
    used = sorted({tid for _, tid, _, _ in tmap})
    renum = {old: i + 1 for i, old in enumerate(used)}
    tiles = [tiles[old - 1] for old in used]
    tmap = [(k, renum[tid], c, f) for k, tid, c, f in tmap]
    return tiles, tmap


def tilemap_section(tmap):
    out = bytearray()
    for key, tid, cat, fl in tmap:
        kb = key.encode('ascii')
        assert len(kb) < 24, key
        out += kb.ljust(24, b'\0') + struct.pack('<HBB', tid, cat, fl)
    return bytes(out)


# ---- SAMSUNG -------------------------------------------------------------------------------

def sam_svg(w, h, k, words):
    """drawSam(ctx, w, h, k) as SVG: 900 x 120 panel px centred; stroke 17, miter (canvas
    miterLimit 10), butt; 'f' letters filled (nonzero)."""
    sc = 900 / 752 * k
    tx, ty = w / 2 - 376 * sc, h / 2 - 50 * sc
    parts = []
    for l in words:
        if l['m'] == 'f':
            parts.append(f'<path transform="translate({l["x"]} 0)" d="{l["d"]}" fill="#fff"/>')
        else:
            parts.append(f'<path transform="translate({l["x"]} 0)" d="{l["d"]}" fill="none" '
                         f'stroke="#fff" stroke-width="17" stroke-linejoin="miter" '
                         f'stroke-miterlimit="10" stroke-linecap="butt"/>')
    return svg_doc(w, h, f'<g transform="translate({tx!r} {ty!r}) scale({sc!r})">{"".join(parts)}</g>',
                   '')


def sam_words():
    html = (ROOT / 'docs/concepts/ios27-boot.html').read_text()
    blk = html[html.index('const WORD=['):html.index('];', html.index('const WORD=['))]
    words = [{'x': float(x), 'm': m, 'd': d}
             for x, m, d in re.findall(r"\{x:\s*(\d+),\s*m:'(\w)',\s*d:'([^']+)'\}", blk)]
    assert len(words) == 7, words
    sw = re.search(r'const SW=(\d+);', html)
    assert sw and sw.group(1) == '17'
    return words


def samsung(words):
    a = rsvg(sam_svg(1300, 220, 1, words), 1300, 220)[..., 3]
    return np.clip(np.rint(a), 0, 255).astype(np.uint8)


def height_field(words):
    """buildHeight(): drawSam(650 x 110, k .5), alpha box-blurred r 3 twice (separable, clamped),
    stored as Uint8ClampedArray does (round half to even)."""
    a = rsvg(sam_svg(650, 110, .5, words), 650, 110)[..., 3]
    a = np.clip(np.rint(a), 0, 255)
    R = 3
    for _ in range(2):
        p = np.pad(a, ((0, 0), (R, R)), mode='edge')
        c = np.cumsum(np.pad(p, ((0, 0), (1, 0))), axis=1)
        a = (c[:, 2 * R + 1:] - c[:, :-2 * R - 1]) / (2 * R + 1)
        p = np.pad(a, ((R, R), (0, 0)), mode='edge')
        c = np.cumsum(np.pad(p, ((1, 0), (0, 0))), axis=0)
        a = (c[2 * R + 1:, :] - c[:-2 * R - 1, :]) / (2 * R + 1)
    return np.clip(np.rint(a), 0, 255).astype(np.uint8)


# ---- the container -------------------------------------------------------------------------

def container(sections):
    """sections: {(type, id): bytes} -> the SB7ASSET container."""
    keys = sorted(sections)
    n = len(keys)
    off = align(32 + 16 * n, 16)
    dir_ = bytearray()
    data = bytearray()
    base = off
    for k in keys:
        b = sections[k]
        pos = base + len(data)
        assert pos % 16 == 0
        dir_ += struct.pack('<IIII', k[0], k[1], pos, len(b))
        data += b
        data += b'\0' * (align(len(data), 16) - len(data))
    pad = b'\0' * (off - 32 - 16 * n)
    body = bytes(dir_) + pad + bytes(data)
    size = 32 + len(body)
    dcrc = zlib.crc32(bytes(dir_)) & 0xffffffff
    tail = pad + bytes(data)
    xcrc = zlib.crc32(tail) & 0xffffffff
    head = b'SB7ASSET' + struct.pack('<IIIIII', 1, n, size, dcrc, xcrc, 0)
    return head + body


ICONS = {   # id: (symbol, size, stroke)
    1: ('i-starf', 40, 2), 2: ('i-starf', 32, 2), 3: ('i-starf', 54, 2), 4: ('i-star', 58, 2),
    5: ('i-check', 58, 2), 6: ('i-check', 54, 2), 7: ('i-play', 58, 2), 8: ('i-info', 58, 2),
    9: ('i-info', 54, 2), 10: ('i-timer', 46, 2), 11: ('i-timer', 54, 2), 12: ('i-flask', 54, 2),
    13: ('i-clock', 36, 2), 14: ('i-life', 74, 1.9), 15: ('i-down', 74, 1.9),
    16: ('i-flask', 74, 1.9), 17: ('i-power', 84, 2.1), 18: ('i-power', 84, 2),
    19: ('i-life', 84, 2), 20: ('i-down', 84, 2), 21: ('i-x', 54, 2.4),
}
PROVIDERS = {64: 'apple', 65: 'win', 66: 'andr', 67: 'tux', 68: 'star', 69: 'flask', 70: 'other'}


def build(fonts_only=False, preview=None, log=print):
    global ART
    secs = {}
    info = [TOOL_VERSION, 'fonts (google/fonts, OFL; see tools/brand/fonts/SOURCES-menu7.md):']
    for fam, fn in FONT_FILES.items():
        h = hashlib.sha256((FONTS / fn).read_bytes()).hexdigest()
        info.append(f'  {fn} sha256 {h} {FONT_URLS[fam]}')
    src = (FONTS / 'SOURCES-menu7.md')
    if src.exists():
        m = re.search(r'commit `([0-9a-f]{40})`', src.read_text())
        if m:
            info.append(f'  google/fonts commit {m.group(1)}')
    info.append(f'  Inter opsz {INTER_OPSZ}, features: kern (tnum on font 6); no cv11/ss01 '
                '(as fonts.googleapis.com served the concept)')
    if CONCEPT_ART:
        info.append("tiles and provider glyphs: the concept's own artwork (--concept-art)")
    else:
        info += menu7_logos.credits()
    stats = {}
    for fid in FONT_SPEC:
        b, st = build_font(fid)
        secs[(T_FONT, fid)] = b
        stats[fid] = st
        fam, wght, px, _, _ = FONT_SPEC[fid]
        log(f'font {fid:2}: fam {fam} {wght} {px}px  {st["glyphs"]} glyphs {st["kern"]} kern '
            f'{st["bytes"] // 1024} KiB' + (f'  missing {st["missing"]!r}' if st['missing'] else ''))
    imgs = {}
    if not fonts_only:
        words = sam_words()
        ART = artwork()
        ART['defs_only'] = ART['defs'].split('</defs>')[0] + '</defs>'
        for iid, (sid, size, sw) in ICONS.items():
            a, nom = icon_a8(sid, size, sw)
            secs[(T_IMAGE, iid)] = image_section(a, A8, nom)
            imgs[iid] = ('a8', a, nom)
        # i-warn 150 (#erIc svg: no class i: the symbol's own fills and strokes; color #ffd60a)
        vb, body = symbol_body('i-warn')
        m = 4
        W = 150 + 2 * m
        inner = (f'<svg x="{m}" y="{m}" width="150" height="150" viewBox="{vb}" overflow="visible" '
                 f'color="#ffd60a">{body}</svg>')
        warn = rsvg(svg_doc(W, W, inner, ''), W, W)
        secs[(T_IMAGE, 22)] = image_section(warn, ARGB, (m, m, 150, 150))
        imgs[22] = ('argb', warn, (m, m, 150, 150))
        # i-bat 84 x 40 (viewBox 44 x 20, color #fff)
        vb, body = symbol_body('i-bat')
        m = 2
        inner = (f'<svg x="{m}" y="{m}" width="84" height="40" viewBox="{vb}" overflow="visible" '
                 f'color="#fff">{body}</svg>')
        bat = rsvg(svg_doc(84 + 2 * m, 40 + 2 * m, inner, ''), 84 + 2 * m, 40 + 2 * m)
        secs[(T_IMAGE, 23)] = image_section(bat, ARGB, (m, m, 84, 40))
        imgs[23] = ('argb', bat, (m, m, 84, 40))
        # wordmark stars (.sbw .sst: .42 em); nominal box in whole px, the item scales it
        for iid, px in ((24, 56), (25, 66)):
            a, nom = glyph_a8('g-star', .42 * px, int(round(.42 * px)))
            secs[(T_IMAGE, iid)] = image_section(a, A8, nom)
            imgs[iid] = ('a8', a, nom)
        for iid, g in PROVIDERS.items():
            rgba, nom = provider_glyph(g)
            secs[(T_IMAGE, iid)] = image_section(rgba, ARGB, nom)
            imgs[iid] = ('argb', rgba, nom)
        sam = samsung(words)
        secs[(T_IMAGE, 80)] = image_section(sam, A8, (0, 0, 1300, 220))
        imgs[80] = ('a8', sam, (0, 0, 1300, 220))
        hf = height_field(words)
        secs[(T_HEIGHT, 0)] = struct.pack('<HHHHHHI', 650, 110, 70, 1270, 1300, 220, 0) + hf.tobytes()
        # tiles
        tiles, tmap = tile_table(ART)
        reqs = []
        for name, c1, c2, g, dark in tiles:
            reqs.append([g, 'dark' if dark else c2])
        tg = artwork([r for r in reqs if not r[0].startswith(('g:', 'L:'))])['tg']
        it = iter(tg)
        layers = {}
        next_img = G_FIRST
        for tid, ((name, c1, c2, g, dark), (gg, bg)) in enumerate(zip(tiles, reqs), 1):
            svg = None if g.startswith('L:') else EXTRA_G[g] if g.startswith('g:') else next(it)
            ids = []
            for si, T in enumerate(TILE_SIZES):
                lay = logo_layer(g, T) if svg is None else tile_layer(svg, T)
                if lay[..., 3].max() == 0:
                    ids.append(0)
                    continue
                secs[(T_IMAGE, next_img)] = image_section(lay, ARGB, (0, 0, T, T))
                layers[(tid, si)] = lay
                ids.append(next_img)
                next_img += 1
            secs[(T_TILE, tid)] = struct.pack('<IIHHHHHH', int(c1.lstrip('#'), 16),
                                              int(c2.lstrip('#'), 16), *ids,
                                              TF_DARK if dark else 0, 0)
        secs[(T_TILEMAP, 0)] = tilemap_section(tmap)
        log(f'tiles: {len(tiles)}, glyph layers: {next_img - G_FIRST}, map entries: {len(tmap)}')
    secs[(T_INFO, 0)] = ('\n'.join(info) + '\n').encode('ascii') + b'\0'
    blob = container(secs)
    out = OUT / 'concept' if CONCEPT_ART else OUT
    out.mkdir(parents=True, exist_ok=True)
    (out / 'menu7.sb7').write_bytes(blob)
    by_type = {}
    for (t, i), b in secs.items():
        by_type[t] = by_type.get(t, 0) + len(b)
    log('container: %d sections, %d bytes (%s)' % (
        len(secs), len(blob), ', '.join(f'type {t}: {v // 1024} KiB' for t, v in sorted(by_type.items()))))
    assert len(blob) <= 8 << 20, 'container over 8 MiB'
    em = (OUT / 'menu-em.sbm').read_bytes()
    rd = em + b'\0' * (align(len(em), 4096) - len(em)) + blob
    assert len(rd) < 15 << 20, 'ramdisk over 15 MiB'
    (out / 'menu-v7.sbm').write_bytes(rd)
    log(f'ramdisk: {(out / "menu-v7.sbm").relative_to(ROOT)} {len(rd)} bytes, SB7ASSET at +0x{align(len(em), 4096):x}')
    if preview and not fonts_only:
        write_previews(Path(preview), imgs, layers, tiles)
    return blob


# ---- previews ------------------------------------------------------------------------------

def sq_mask(T, ss=4, grow=0.0):
    """The squircle n = 5 as the concept's 72-segment polygon, anti-aliased (supersampled);
    grow: units (of 100) added to the radius (the rim's preview approximation)."""
    from PIL import ImageDraw
    pts = []
    R = 50 + grow
    for i in range(72):
        a = i / 72 * 2 * np.pi
        c, s = np.cos(a), np.sin(a)
        pts.append(((50 + R * np.sign(c) * abs(c) ** .4) * T / 100 * ss,
                    (50 + R * np.sign(s) * abs(s) ** .4) * T / 100 * ss))
    im = Image.new('L', (T * ss, T * ss), 0)
    ImageDraw.Draw(im).polygon(pts, fill=255)
    return np.asarray(im.resize((T, T), Image.LANCZOS)).astype(np.float64)


def tile_preview(c1, c2, lay, T):
    """What glass draws for a tile (sb7.h), as the concept's SVG: gradient (0,0)->(.75,1) of the
    box, glyph layer, sheen (tsheen: SVG interpolates colour and opacity separately, white .28 ->
    white 0 at .45 -> black .14), the 1.1-unit rim white .24 on the outline, clipped to the
    squircle (the rim's outer half outside it)."""
    y, x = np.mgrid[0:T, 0:T] + .5
    dx, dy = .75, 1.0
    t = np.clip(((x / T) * dx + (y / T) * dy) / (dx * dx + dy * dy), 0, 1)[..., None]
    a, b = np.array(hexrgb(c1)), np.array(hexrgb(c2))
    base = np.concatenate([a + (b - a) * t, np.full((T, T, 1), 255.0)], axis=-1)
    out = over(base, lay)
    v = y / T
    sh = np.zeros((T, T, 4))
    u = np.clip((v - .45) / .55, 0, 1)
    col = np.where(v < .45, 255.0, 255.0 * (1 - u))
    al = np.where(v < .45, .28 * (1 - v / .45), .14 * u)
    sh[..., 0] = sh[..., 1] = sh[..., 2] = col
    sh[..., 3] = al * 255
    out = over(out, sh)
    mask = sq_mask(T)
    out[..., 3] = mask
    rim = np.clip(sq_mask(T, grow=.55) - sq_mask(T, grow=-.55), 0, 255)
    rr = np.zeros((T, T, 4))
    rr[..., :3] = 255
    rr[..., 3] = rim * .24
    return over(out, rr)


def write_previews(d, imgs, layers, tiles):
    d.mkdir(parents=True, exist_ok=True)
    # icons on dark grey
    cells = []
    for iid, (kind, a, nom) in sorted(imgs.items()):
        if iid == 80:
            continue
        if kind == 'a8':
            rgba = np.zeros(a.shape + (4,))
            rgba[..., :3] = 255
            rgba[..., 3] = a
        else:
            rgba = a
        cells.append(rgba)
    W = sum(c.shape[1] + 16 for c in cells) + 16
    H = max(c.shape[0] for c in cells) + 32
    sheet = np.zeros((H, W, 4))
    sheet[..., :3] = (40, 44, 56)
    sheet[..., 3] = 255
    x = 16
    for c in cells:
        h, w = c.shape[:2]
        sheet[16:16 + h, x:x + w] = over(sheet[16:16 + h, x:x + w], c)
        x += w + 16
    Image.fromarray(np.clip(sheet, 0, 255).astype(np.uint8)).save(d / 'icons.png')
    # tiles at 124 and 380
    for si, T in ((1, 124), (3, 380)):
        cols = 8
        rows = (len(tiles) + cols - 1) // cols
        g = 24
        sheet = np.zeros((rows * (T + g) + g, cols * (T + g) + g, 4))
        sheet[..., :3] = (20, 24, 40)
        sheet[..., 3] = 255
        for i, (name, c1, c2, gl, dark) in enumerate(tiles):
            lay = layers.get((i + 1, si), np.zeros((T, T, 4)))
            tp = tile_preview(c1, c2, lay, T)
            yy, xx = g + (i // cols) * (T + g), g + (i % cols) * (T + g)
            sheet[yy:yy + T, xx:xx + T] = over(sheet[yy:yy + T, xx:xx + T], tp)
        Image.fromarray(np.clip(sheet, 0, 255).astype(np.uint8)).save(d / f'tiles-{T}.png')
    Image.fromarray(imgs[80][1]).save(d / 'samsung.png')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--fonts-only', action='store_true')
    ap.add_argument('--preview')
    ap.add_argument('--concept-art', action='store_true')
    args = ap.parse_args()
    global CONCEPT_ART
    CONCEPT_ART = args.concept_art
    build(args.fonts_only, args.preview)
    if not args.fonts_only and not CONCEPT_ART:
        subprocess.run([sys.executable, str(ROOT / 'tools/brand/menu7-splash.py')], check=True)


if __name__ == '__main__':
    main()
