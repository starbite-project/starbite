#!/usr/bin/env python3
"""sweep-diff.py OURS REF [OUT]: the T37 sweep (docs/WIN11-GPU.md 6.46) on our driver against the reference.

OURS and REF are gpu-cycle.sh OUT folders (OURS: a normal cycle with a sweep stage; REF: the same stage with
--reference, Windows drawing with WARP). For each surface NAME of the sweep:
  ref    REF/pre/umdlog/shot-NAME.ppm          (GDI screenshot, quarter size)
  ours   OURS/pre/umdlog/dump-dwm-NAME-p*.ppm  (DWM's composed frame, half size: what the panel showed), scaled to
         quarter; else OURS' own shot-NAME.ppm
  shot   OURS/pre/umdlog/shot-NAME.ppm         (our GDI screenshot: compared with our DWM dump, a capture bug if
         they differ)
Writes OUT (default OURS/sweep) with NAME.png (ref | ours | difference) and report.md: surfaces ranked by how much
of the screen differs, black-hole share (areas the reference draws and ours leaves near-black), the sbshow lines
and the crash events (Application 1000/1001/1002, System 4101/141/117/41) that fall in each surface's time slot.
Pure Python (no PIL).
"""
import glob
import os
import re
import struct
import sys
import zlib
from datetime import datetime, timedelta


def read_ppm(path):
    data = open(path, 'rb').read()
    vals, i = [], 0
    while len(vals) < 4:                  # magic, width, height, maxval, separated by whitespace (no comments here)
        while data[i:i + 1].isspace():
            i += 1
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        vals.append(data[i:j])
        i = j
    if vals[0] != b'P6':
        raise ValueError(path)
    w, h = int(vals[1]), int(vals[2])
    return w, h, data[i + 1:i + 1 + w * h * 3]


def scale_to(img, w2, h2):
    w, h, px = img
    if (w, h) == (w2, h2):
        return img
    out = bytearray(w2 * h2 * 3)
    fx, fy = w / w2, h / h2
    for y in range(h2):
        y0, y1 = int(y * fy), max(int(y * fy) + 1, int((y + 1) * fy))
        for x in range(w2):
            x0, x1 = int(x * fx), max(int(x * fx) + 1, int((x + 1) * fx))
            r = g = b = n = 0
            for yy in range(y0, min(y1, h)):
                base = (yy * w) * 3
                for xx in range(x0, min(x1, w)):
                    i = base + xx * 3
                    r += px[i]; g += px[i + 1]; b += px[i + 2]; n += 1
            o = (y * w2 + x) * 3
            if n:
                out[o], out[o + 1], out[o + 2] = r // n, g // n, b // n
    return w2, h2, bytes(out)


def write_png(path, w, h, rgb):
    raw = b''.join(b'\x00' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def compare(a, b, tile=12):
    """Tile statistics: share of tiles that differ (mean abs > 24), share of 'black holes' (ref bright, ours dark)."""
    w, h, pa = a
    _, _, pb = b
    tiles = bad = holes = 0
    heat = bytearray(w * h * 3)
    for ty in range(0, h - tile + 1, tile):
        for tx in range(0, w - tile + 1, tile):
            d = la = lb = 0
            for y in range(ty, ty + tile):
                for x in range(tx, tx + tile):
                    i = (y * w + x) * 3
                    d += abs(pa[i] - pb[i]) + abs(pa[i + 1] - pb[i + 1]) + abs(pa[i + 2] - pb[i + 2])
                    la += pa[i] + pa[i + 1] + pa[i + 2]
                    lb += pb[i] + pb[i + 1] + pb[i + 2]
            n = tile * tile
            d, la, lb = d / (3 * n), la / (3 * n), lb / (3 * n)
            tiles += 1
            if d > 24:
                bad += 1
            if la > 40 and lb < 12:
                holes += 1
            v = min(255, int(d * 3))
            for y in range(ty, ty + tile):
                for x in range(tx, tx + tile):
                    i = (y * w + x) * 3
                    heat[i] = v
                    heat[i + 1] = v // 4
                    heat[i + 2] = 0
    return (bad / tiles if tiles else 0.0), (holes / tiles if tiles else 0.0), bytes(heat)


def side_by_side(imgs, gap=6):
    w, h = imgs[0][0], imgs[0][1]
    W = w * len(imgs) + gap * (len(imgs) - 1)
    out = bytearray(b'\x30' * (W * h * 3))
    for k, (_, _, px) in enumerate(imgs):
        ox = k * (w + gap)
        for y in range(h):
            out[(y * W + ox) * 3:(y * W + ox + w) * 3] = px[y * w * 3:(y + 1) * w * 3]
    return W, h, bytes(out)


def sweep_times(log):
    """'sbshow snap NAME at HH:MM:SS' -> {NAME: seconds of day}, in sweep order."""
    times = {}
    if not os.path.exists(log):
        return times
    for line in open(log, 'rb').read().decode('latin-1').splitlines():
        m = re.match(r'sbshow snap (\S+) at (\d+):(\d+):(\d+)', line.strip())
        if m:
            times[m.group(1)] = int(m.group(2)) * 3600 + int(m.group(3)) * 60 + int(m.group(4))
    return times


def events(umd):
    """Crash-ish events from the sweep's wevtutil dumps: (seconds of day, text)."""
    out = []
    for name in ('sweep-app-events.txt', 'sweep-sys-events.txt'):
        p = os.path.join(umd, name)
        if not os.path.exists(p):
            continue
        txt = open(p, 'rb').read().decode('utf-16' if open(p, 'rb').read(2) == b'\xff\xfe' else 'latin-1', 'replace')
        for block in re.split(r'\nEvent\[\d+\]', '\n' + txt):
            m = re.search(r'Date:\s*(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d)', block)
            eid = re.search(r'Event ID:\s*(\d+)', block)
            if not m or not eid:
                continue
            t = datetime.strptime(m.group(1), '%Y-%m-%dT%H:%M:%S')
            desc = re.search(r'Description:\s*\n?(.*?)(?:\n\s*\n|$)', block, re.S)
            d = ' '.join((desc.group(1) if desc else '').split())[:220]
            out.append((t, int(eid.group(1)), d))
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    ours, ref = sys.argv[1], sys.argv[2]
    out = sys.argv[3] if len(sys.argv) > 3 else os.path.join(ours, 'sweep')
    os.makedirs(out, exist_ok=True)
    ou, ru = os.path.join(ours, 'pre', 'umdlog'), os.path.join(ref, 'pre', 'umdlog')
    names = sorted({os.path.basename(p)[5:-4] for p in glob.glob(os.path.join(ru, 'shot-*.ppm'))} |
                   {os.path.basename(p)[5:-4] for p in glob.glob(os.path.join(ou, 'shot-*.ppm'))})
    times = sweep_times(os.path.join(ours, 'pre', 'gputest', 'gputest.log'))
    evs = events(ou)
    rows = []
    for name in names:
        rp = os.path.join(ru, 'shot-%s.ppm' % name)
        dumps = sorted(glob.glob(os.path.join(ou, 'dump-dwm-%s-p*.ppm' % name)))
        sp = os.path.join(ou, 'shot-%s.ppm' % name)
        if not os.path.exists(rp):
            rows.append((name, None, None, None, 'no reference shot'))
            continue
        r = read_ppm(rp)
        src = dumps[0] if dumps else (sp if os.path.exists(sp) else None)
        if not src:
            rows.append((name, 1.0, 1.0, None, 'no shot or DWM dump from our driver (the step failed?)'))
            continue
        o = scale_to(read_ppm(src), r[0], r[1])
        bad, holes, heat = compare(r, o)
        cap = None
        if dumps and os.path.exists(sp):
            s = scale_to(read_ppm(sp), r[0], r[1])
            cap, _, _ = compare(o, s)
        trip = side_by_side([r, o, (r[0], r[1], heat)])
        write_png(os.path.join(out, name + '.png'), *trip)
        rows.append((name, bad, holes, cap, os.path.basename(src)))
    rows.sort(key=lambda r: -(r[1] or 0) - 2 * (r[2] or 0))
    order = list(times.items())
    lines = ['# Sweep: our driver against the reference (WARP)', '',
             'ours: `%s`  ref: `%s`  images: `%s/<surface>.png` (reference | ours | difference)' % (ours, ref, out), '',
             '| rank | surface | screen differing | black holes | GDI shot vs DWM | events in its slot | source |',
             '|---|---|---|---|---|---|---|']
    for i, (name, bad, holes, cap, src) in enumerate(rows, 1):
        slot = []
        if name in times:
            k = [n for n, _ in order].index(name)
            t0 = times[name] - 15
            t1 = order[k + 1][1] if k + 1 < len(order) else times[name] + 60
            for t, eid, d in evs:
                s = t.hour * 3600 + t.minute * 60 + t.second
                if t0 <= s < t1:
                    slot.append('%d %s' % (eid, d[:90]))
        lines.append('| %d | %s | %s | %s | %s | %s | %s |' % (
            i, name, '-' if bad is None else '%.0f%%' % (100 * bad), '-' if holes is None else '%.0f%%' % (100 * holes),
            '-' if cap is None else '%.0f%%' % (100 * cap), '<br>'.join(slot) or '', src))
    lines += ['', '## Crash events during the sweep (all)', '']
    if times:
        lo, hi = min(times.values()) - 60, max(times.values()) + 120
        for t, eid, d in evs:
            s = t.hour * 3600 + t.minute * 60 + t.second
            if lo <= s <= hi:
                lines.append('- %s  %d  %s' % (t.strftime('%H:%M:%S'), eid, d))
    open(os.path.join(out, 'report.md'), 'w').write('\n'.join(lines) + '\n')
    print('\n'.join(lines))
    return 0


if __name__ == '__main__':
    sys.exit(main())
