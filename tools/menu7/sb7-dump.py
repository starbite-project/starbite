#!/usr/bin/env python3
"""Check and inspect a menu v7 asset container ("SB7ASSET", shim/starlte/sb7.h).

    python3 tools/menu7/sb7-dump.py [FILE] [--check-geometry] [--sheets DIR] [-v]

FILE is build/brand/menu7.sb7 (default) or a ramdisk holding one at a 4 KiB boundary
(build/brand/menu-v7.sbm). Always: header, both CRC-32s, the directory (sorted, aligned, in
range), every section's internal offsets, every RLE stream decoded to exactly w * h pixels.
--check-geometry: every text of docs/concepts/boot-geometry/*.json measured with the container's
advances, kerning and letter-spacing (as g7_text_w) against the concept's `tw` (0.5 px).
--sheets DIR: PNGs of every font (glyph sheet + a sample line), every image, the height field.
Plain Python 3 (struct, zlib); Pillow only for --sheets.
"""
import argparse
import glob
import json
import re
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
T_NAMES = {1: 'FONT', 2: 'IMAGE', 3: 'TILE', 4: 'TILEMAP', 5: 'HEIGHT', 6: 'INFO'}
FAMS = {1: 'Inter Tight', 2: 'Inter', 3: 'JetBrains Mono'}


class Bad(Exception):
    pass


def need(c, msg):
    if not c:
        raise Bad(msg)


def find(buf):
    if buf[:8] == b'SB7ASSET':
        return 0
    for off in range(0, len(buf), 4096):
        if buf[off:off + 8] == b'SB7ASSET':
            return off
    raise Bad('no SB7ASSET at a 4 KiB boundary')


class Font:
    def __init__(self, b):
        (self.px, self.weight, self.family, n, self.asc64, self.desc64, self.gap64, nk, self.flags,
         go, ko, bo, bs) = struct.unpack_from('<HHHHiiiHHIIII', b, 0)
        need(go + 20 * n <= len(b) and ko + 8 * nk <= len(b) and bo + bs <= len(b), 'font tables out of range')
        self.glyphs = []
        last = -1
        for i in range(n):
            cp, adv, w, h, bx, by, off = struct.unpack_from('<IiHHhhI', b, go + 20 * i)
            need(cp > last, 'glyphs not sorted by code point')
            last = cp
            need(off + w * h <= bs, 'glyph bitmap out of range')
            self.glyphs.append((cp, adv, w, h, bx, by, b[bo + off:bo + off + w * h]))
        self.index = {g[0]: i for i, g in enumerate(self.glyphs)}
        self.kern = {}
        lastk = (-1, -1)
        for i in range(nk):
            l, r, a = struct.unpack_from('<HHi', b, ko + 8 * i)
            need((l, r) > lastk and l < n and r < n, 'kerning not sorted or out of range')
            lastk = (l, r)
            self.kern[(l, r)] = a

    def width(self, s, track_px):
        """g7_text_w: advances + kerning (1/64 px) + letter-spacing after every character."""
        pen = 0
        prev = None
        miss = ''
        for ch in s:
            i = self.index.get(ord(ch))
            if i is None:
                miss += ch
                prev = None
                continue
            if prev is not None:
                pen += self.kern.get((prev, i), 0)
            pen += self.glyphs[i][1]
            prev = i
        return pen / 64 + track_px * len(s), miss


def unrle(data, count, bpp):
    out = bytearray()
    i = 0
    while len(out) < count * bpp:
        need(i + 2 <= len(data), 'RLE stream ends early')
        c, = struct.unpack_from('<H', data, i)
        i += 2
        if c >= 0x8000:
            out += data[i:i + bpp] * (c - 0x8000)
            i += bpp + (bpp & 1)
        else:
            out += data[i:i + c * bpp]
            i += c * bpp + ((c * bpp) & 1)
    need(len(out) == count * bpp, 'RLE overruns the image')
    need(i == len(data), 'RLE has trailing bytes')
    return bytes(out)


def image(b):
    w, h, fmt, enc, fl, nx, ny, nw, nh, ds = struct.unpack_from('<HHBBHHHHHI', b, 0)
    need(20 + ds <= len(b), 'image data out of range')
    need(fmt in (1, 2) and enc in (0, 1), 'bad image format')
    need(nx + nw <= w and ny + nh <= h, 'nominal box outside the bitmap')
    bpp = 1 if fmt == 1 else 4
    d = b[20:20 + ds]
    px = unrle(d, w * h, bpp) if enc == 1 else d
    need(len(px) == w * h * bpp, 'raw image size')
    return dict(w=w, h=h, fmt=fmt, enc=enc, nom=(nx, ny, nw, nh), px=px, size=ds)


def parse(buf):
    base = find(buf)
    c = buf[base:]
    need(len(c) >= 32, 'short')
    ver, n, size, dcrc, xcrc, flags = struct.unpack_from('<IIIIII', c, 8)
    need(ver == 1, f'version {ver}')
    need(size <= len(c), 'size beyond the file')
    c = c[:size]
    dir_ = c[32:32 + 16 * n]
    need(zlib.crc32(dir_) & 0xffffffff == dcrc, 'directory CRC')
    need(zlib.crc32(c[32 + 16 * n:]) & 0xffffffff == xcrc, 'data CRC')
    secs = {}
    last = (0, -1)
    for i in range(n):
        t, sid, off, sz = struct.unpack_from('<IIII', dir_, 16 * i)
        need((t, sid) > last, 'directory not sorted')
        last = (t, sid)
        need(off % 16 == 0 and off >= 32 + 16 * n and off + sz <= size, f'section {t}/{sid} range')
        secs[(t, sid)] = c[off:off + sz]
    return base, size, secs


def font_key(desc):
    m = re.match(r'"?([^"]+?)"? (\d+) ([\d.]+)px ls=(\S+) ', desc)
    fam, w, px, ls = m.group(1), int(m.group(2)), float(m.group(3)), m.group(4)
    ls = 0.0 if ls == 'normal' else float(ls[:-2])
    return fam, w, px, ls


def check_geometry(fonts, verbose):
    lut = {}
    for fid, f in fonts.items():
        lut.setdefault((FAMS[f.family], f.weight, f.px), fid)
    tot = ok = scaled = prefix = skipped = 0
    bad = []
    for fn in sorted(glob.glob(str(ROOT / 'docs/concepts/boot-geometry/*.json'))):
        st = Path(fn).stem
        d = json.load(open(fn))
        scales = {1.0}
        for e in d:
            m = re.match(r'matrix\(([-\d.e]+), [-\d.e]+, [-\d.e]+, ([-\d.e]+)', e.get('tf', ''))
            if m and abs(float(m.group(1)) - float(m.group(2))) < 1e-6 and float(m.group(1)) > 0:
                scales.add(float(m.group(1)))
        for e in d:
            if 'text' not in e or 'font' not in e:
                continue
            fam, w, px, ls = font_key(e['font'])
            if fam == 'JetBrains Mono':
                w = 500                                     # the only weight the concept loads
            fid = lut.get((fam, w, px))
            if fid is None or e['p'].endswith('svg > text'):
                skipped += 1                                # tile glyph text (baked into tiles)
                continue
            tot += 1
            W, miss = fonts[fid].width(e['text'], ls)
            tw = e['tw']
            if miss:
                bad.append((st, fid, e['text'], W, tw, 'missing ' + miss))
            elif abs(W - tw) <= .5:
                ok += 1
            elif any(abs(W * s - tw) <= .5 for s in scales):
                scaled += 1                                 # under a scaled ancestor (#menu .965 ...)
            elif any(abs(fonts[fid].width(e['text'][:k], ls)[0] * s - tw) <= .5
                     for k in range(1, len(e['text'])) for s in scales):
                prefix += 1                                 # several text nodes: tw is the first's
            else:
                bad.append((st, fid, e['text'], W, tw, ''))
    print(f'geometry: {tot} texts: {ok} exact, {scaled} under a scale transform, {prefix} '
          f'first-text-node, {len(bad)} FAIL; {skipped} tile glyph texts skipped')
    for st, fid, t, W, tw, why in bad:
        print(f'  FAIL {st}: font {fid} {t!r}: {W:.2f} vs tw {tw} {why}')
    return not bad


def sheets(d, fonts, secs):
    from PIL import Image
    d.mkdir(parents=True, exist_ok=True)
    for fid, f in sorted(fonts.items()):
        pad = 8
        lh = (f.asc64 + f.desc64) // 64 + pad
        cols = 16
        cw = max([g[2] for g in f.glyphs] + [f.px // 2]) + pad
        rows = (len(f.glyphs) + cols - 1) // cols
        W, H = cols * cw + pad, (rows + 1) * lh + pad
        im = Image.new('L', (W, H), 0)
        for i, (cp, adv, w, h, bx, by, bits) in enumerate(f.glyphs):
            if not w:
                continue
            g = Image.frombytes('L', (w, h), bits)
            x = pad + (i % cols) * cw
            y = pad + (i // cols) * lh + f.asc64 // 64
            im.paste(255, (x + max(bx, 0), y + by), g)
        # a sample line at whole-pixel origins, as glass draws at scale 1
        s = {1: 'Choose a System', 2: 'Apple Microsoft Google Linux', 6: '0123456789',
             10: 'Galaxy S9', 11: 'starbite', 12: 'starbite', 26: 'Powered by'}.get(
            fid, 'Ubuntu 26.04 LTS · Couldn’t Start “Arch” … 0x9A3F1C07 ✓')
        pen = pad * 64
        yb = H - pad - f.desc64 // 64
        prev = None
        for ch in s:
            i = f.index.get(ord(ch))
            if i is None:
                continue
            if prev is not None:
                pen += f.kern.get((prev, i), 0)
            cp, adv, w, h, bx, by, bits = f.glyphs[i]
            ox = (pen + 32) >> 6
            if w and ox + bx + w < W:
                im.paste(255, (ox + bx, yb + by), Image.frombytes('L', (w, h), bits))
            pen += adv
            prev = i
        im.save(d / f'font-{fid:02}.png')
    for (t, i), b in sorted(secs.items()):
        if t != 2 or i >= 256:
            continue
        m = image(b)
        if m['fmt'] == 1:
            Image.frombytes('L', (m['w'], m['h']), m['px']).save(d / f'image-{i:03}.png')
        else:
            Image.frombytes('RGBA', (m['w'], m['h']), m['px'], 'raw', 'BGRA').save(d / f'image-{i:03}.png')
    if (5, 0) in secs:
        b = secs[(5, 0)]
        w, h = struct.unpack_from('<HH', b, 0)
        Image.frombytes('L', (w, h), b[16:16 + w * h]).save(d / 'height.png')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file', nargs='?', default=str(ROOT / 'build/brand/menu7.sb7'))
    ap.add_argument('--check-geometry', action='store_true')
    ap.add_argument('--sheets')
    ap.add_argument('-v', action='store_true')
    a = ap.parse_args()
    buf = Path(a.file).read_bytes()
    try:
        base, size, secs = parse(buf)
        fonts, images, counts = {}, {}, {}
        for (t, i), b in secs.items():
            counts[t] = counts.get(t, 0) + 1
            if t == 1:
                fonts[i] = Font(b)
            elif t == 2:
                images[i] = image(b)
            elif t == 3:
                need(len(b) == 20, 'tile record size')
                c1, c2, *g, fl, pad = struct.unpack('<IIHHHHHH', b)
                for gi in g:
                    need(gi == 0 or (2, gi) in secs, f'tile {i}: glyph layer {gi} missing')
            elif t == 4:
                need(len(b) % 28 == 0, 'tile map size')
                for k in range(len(b) // 28):
                    key = b[28 * k:28 * k + 24].split(b'\0')[0].decode()
                    tid, cat, fl = struct.unpack_from('<HBB', b, 28 * k + 24)
                    need((3, tid) in secs, f'tile map {key!r} -> missing tile {tid}')
                    if a.v:
                        print(f'  map {key or "(category " + str(cat) + ")":24} -> tile {tid}'
                              + (' prefix' if fl & 1 else ''))
            elif t == 5:
                w, h, fx, fy, fw, fh = struct.unpack_from('<HHHHHH', b, 0)
                need(len(b) == 16 + w * h, 'height field size')
            elif t == 6:
                need(b.endswith(b'\0'), 'INFO not NUL terminated')
                if a.v:
                    print(b[:-1].decode())
    except Bad as e:
        print(f'{a.file}: INVALID: {e}')
        return 1
    print(f'{a.file}: SB7ASSET at +0x{base:x}, {size} bytes, {len(secs)} sections: ' +
          ', '.join(f'{counts[t]} {T_NAMES.get(t, t)}' for t in sorted(counts)) + '; CRCs OK')
    if a.v:
        for fid, f in sorted(fonts.items()):
            print(f'  font {fid:2}: {FAMS[f.family]} {f.weight} {f.px}px, {len(f.glyphs)} glyphs, '
                  f'{len(f.kern)} kern pairs, asc {f.asc64 / 64:.2f} desc {f.desc64 / 64:.2f}')
        for i, m in sorted(images.items()):
            if i < 256:
                print(f'  image {i:3}: {m["w"]}x{m["h"]} {"A8" if m["fmt"] == 1 else "ARGB"} '
                      f'{"RLE" if m["enc"] else "raw"} {m["size"]} bytes, nominal {m["nom"]}')
    rc = 0
    if a.check_geometry and not check_geometry(fonts, a.v):
        rc = 1
    if a.sheets:
        sheets(Path(a.sheets), fonts, secs)
    return rc


if __name__ == '__main__':
    sys.exit(main())
