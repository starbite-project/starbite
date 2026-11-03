#!/usr/bin/env python3
"""Turn a concept geometry dump (docs/concepts/boot-geometry/<state>.json) into a glass display
list for h7 (tools/menu7/preview/h7.c): the concept's exact scene with no menu7, for the glass
renderer's own diff against the concept (docs/MENU7-PORT.md section 5).

    geom2dl.py docs/concepts/boot-geometry/menu.json > menu.dl
    geom2dl.py --state ctx ctx.json > ctx.dl        (the state decides back and the focus lens)

What maps to what:
  [data-glass] elements      shapes (data-r scaled by the element's transform, clip/frost/tint/s);
                             base layer before #over in document order, over layer after it
  the focus lens             from the state's focused key and the concept's lensTarget() insets
  text nodes                 G7_TEXT (font by family / weight / size; .pn -> G7_I_GRAD; .rn, .dd ->
                             G7_I_ELLIP); OS tile glyph text is part of the tile, skipped
  svg <use> icons            G7_IMAGE by symbol and nearest nominal size; i-chev -> G7_STROKE
  OS tile <svg>s             G7_TILE by the concept entry id (the container's tile map has them)
  backgrounds, borders       G7_RECT (filled) / ring; box-shadow -> G7_SHADOW
  #vp content                G7_I_FADE, clipped to its section's .rows box when inside one
The dump records boxes after transforms and opacity products, not the springs' _ga, so glass alpha
is the element's opacity: close to, not exactly, what the concept's collect() used.
"""
import json
import re
import sys

FONTS = {
    ('Inter Tight', 700, 142): 1, ('Inter Tight', 700, 128): 2, ('Inter Tight', 700, 100): 3,
    ('Inter Tight', 700, 92): 4, ('Inter Tight', 700, 70): 5, ('Inter Tight', 700, 40): 6,
    ('Inter Tight', 600, 66): 7, ('Inter Tight', 600, 54): 8, ('Inter Tight', 600, 46): 9,
    ('Inter Tight', 500, 60): 10, ('Inter Tight', 300, 66): 11, ('Inter Tight', 300, 56): 12,
    ('Inter', 500, 58): 13, ('Inter', 600, 58): 14, ('Inter', 600, 56): 15, ('Inter', 600, 46): 16,
    ('Inter', 600, 33): 17, ('Inter', 400, 50): 18, ('Inter', 400, 48): 19, ('Inter', 400, 46): 20,
    ('Inter', 400, 44): 21, ('Inter', 400, 42): 22, ('Inter', 400, 40): 23, ('Inter', 400, 38): 24,
    ('Inter', 400, 36): 25, ('Inter', 400, 31): 26, ('JetBrains Mono', 500, 40): 27,
    ('JetBrains Mono', 500, 36): 28, ('JetBrains Mono', 500, 34): 29,
}
# symbol -> [(nominal size, image id)]
ICONS = {
    '#i-starf': [(40, 1), (32, 2), (54, 3)], '#i-star': [(58, 4)], '#i-check': [(58, 5), (54, 6)],
    '#i-play': [(58, 7)], '#i-info': [(58, 8), (54, 9)], '#i-timer': [(46, 10), (54, 11)],
    '#i-flask': [(54, 12), (74, 16)], '#i-clock': [(36, 13)], '#i-life': [(74, 14), (84, 19)],
    '#i-down': [(74, 15), (84, 20)], '#i-power': [(84, 17)], '#i-x': [(54, 21)], '#i-warn': [(150, 22)],
    '#i-bat': [(84, 23)], '#g-star': [(23.52, 24), (27.72, 25), (96, 68)],
    '#g-apple': [(96, 64)], '#g-win': [(96, 65)], '#g-andr': [(96, 66)], '#g-tux': [(96, 67)],
    '#g-flask': [(96, 69)],
}
CAT = {'apple': 0, 'microsoft': 1, 'google': 2, 'linux': 3, 'starbite': 4, 'lab': 6}
BACK1 = {'ctx', 'submenu', 'sheet', 'alert', 'recovery', 'launch', 'error'}
# the focused key per state (base lens LB / over lens LO), as go(st) leaves it
FOCUS = {
    'menu': ('base', 'r:linux:phosh'), 'countdown': ('base', 'r:linux:phosh'), 'collapsed': ('base', 'p:linux'),
    'apple': ('base', 'r:apple:ios26'), 'missing': ('base', 'r:linux:arch'), 'lab': ('base', 'r:linux:phosh'),
    'ctx': ('over', 'm:start'), 'submenu': ('over', 'm:auto:5'), 'sheet': ('over', 'sh:start'),
    'alert': ('over', 'a:cancel'), 'recovery': ('over', 'a:cancel'), 'error': ('over', 'e:retry'),
}


def rgba(s):
    m = re.match(r'rgba?\(([^)]*)\)', s or '')
    if not m:
        return 0
    v = [float(x) for x in m.group(1).split(',')]
    a = v[3] if len(v) > 3 else 1.0
    return (round(a * 255) << 24) | (round(v[0]) << 16) | (round(v[1]) << 8) | round(v[2])


def tf_scale_rot(tf):
    m = re.match(r'matrix\(([^)]*)\)', tf or '')
    if not m:
        return 1.0, 0.0, 1.0, 0.0
    a, b, c, d, e, f = [float(x) for x in m.group(1).split(',')]
    import math
    return math.hypot(a, b), math.atan2(b, a), a, b


def main():
    args = sys.argv[1:]
    state = None
    if args and args[0] == '--state':
        state = args[1]
        args = args[2:]
    path = args[0]
    if not state:
        state = re.sub(r'\.json$', '', path.split('/')[-1])
    d = json.load(open(path))
    over_at = next((i for i, o in enumerate(d) if o['p'] in ('#over', '#cvOver')), None)
    if over_at is None:
        over_at = next((i for i, o in enumerate(d) if o['p'].startswith(('#splash', '#end'))), len(d))
    rows_boxes = [o['r'] for o in d if o['p'].endswith('.rows') or o['p'].endswith('> .rows')]
    lay = {'base': {'shapes': [], 'items': [], 'clips': []}, 'over': {'shapes': [], 'items': [], 'clips': []}}
    key = [1]

    def nk():
        key[0] += 1
        return key[0]

    def clip_for(L, box):
        for i, c in enumerate(L['clips']):
            if c == box:
                return i + 1
        L['clips'].append(box)
        return len(L['clips'])

    last_row = None
    for idx, o in enumerate(d):
        L = lay['over' if idx > over_at else 'base']
        p, r, a = o['p'], o['r'], o.get('a', 1)
        if o.get('f', '').startswith('r:'):
            last_row = o['f']
        if p.startswith(('#ctxPrev', '#over > #ctxPrev')) or 'pill' in p or 'shTile' in p or 'lnGlyph' in p:
            last_row = 'r:linux:phosh'
        comps = [c.strip() for c in p.split('>')]
        cyy = r['y'] + r['h'] / 2
        in_vp = idx < over_at and not any(t in p for t in (
            '#status', '.bat', '#pill', '.pt', '.ring', '#ringN', '#ringT', '#ringC', '#toolbar', '#tbCap', '#tbPow',
            '.tb', '#cvBase', '#menu > #vp', '#menu >'))
        if p in ('#menu > #vp',) or p.startswith('#menu > #vp >'):
            in_vp = True
        flags = 1 if in_vp else 0
        clip = 0
        if in_vp and any(c.startswith(('.rows', '.rowsIn', '.row')) for c in comps):
            cy = r['y'] + r['h'] / 2
            for rb in rows_boxes:
                if rb['y'] <= cy <= rb['y'] + rb['h'] and rb['h'] > 0:
                    clip = clip_for(L, (rb['x'], rb['y'], rb['x'] + rb['w'], rb['y'] + rb['h']))
                    break
        if a <= 0.002:
            continue
        g = o.get('glass')
        if g is not None:
            sc = tf_scale_rot(o.get('tf'))[0]
            rr = float(g.get('r') or 40) * sc
            fl = (1 if g.get('clip') == '1' else 0) | (4 if g.get('frost') == '1' else 0)
            tint = [float(x) for x in g['tint'].split(',')] if g.get('tint') else [.02, .035, .08, .22]
            s = float(g.get('s') or 1)
            L['shapes'].append(f"shape {nk():#x} {fl} {r['x']} {r['y']} {r['w']} {r['h']} {rr:.2f} {a} {s} "
                               + ' '.join(str(t) for t in tint))
        if 'text' in o and 'svg >' not in p and '> text' not in p:
            m = re.match(r'"?([^"]+?)"? (\d+) ([\d.]+)px ls=(\S+)', o['font'])
            fam, wt, px, ls = m.group(1), int(m.group(2)), round(float(m.group(3))), m.group(4)
            fid = FONTS.get((fam, wt, px))
            if fid:
                tr = 0.0 if ls == 'normal' else float(ls.rstrip('px')) / px
                col = rgba(o['color'])
                ifl = flags
                c2, gy0, gy1, gs = 0, 0, 0, 0
                if (col >> 24) == 0 and '.pn' in comps[-1]:
                    col, c2, gy0, gy1, gs, ifl = 0xffffffff, 0xffd9e2f0, r['y'], r['y'] + r['h'], .48, ifl | 2
                mw = 0
                if comps[-1].startswith(('.rn', '.dd')):
                    ifl |= 8
                    mw = r['w']
                L['items'].append(f"text {nk():#x} {clip} {ifl} {a} {col:#010x} {fid} {o['tx']} {o['base']} {tr:.5f} 1 {mw} "
                                  f"{c2:#x} {gy0} {gy1} {gs} |{o['text']}")
        if o.get('svg') and not p.endswith('use'):
            sym = o['svg']
            if sym == '#i-chev':
                import math
                _, rot, _, _ = tf_scale_rot(o.get('tf'))
                k = r['w'] / 24.0 if not o.get('tf') else 2.5
                cx, cy = r['x'] + r['w'] / 2, r['y'] + r['h'] / 2
                pts = []
                for (ux, uy) in ((9, 5), (16, 12), (9, 19)):
                    x, y = (ux - 12) * k, (uy - 12) * k
                    pts.append((cx + x * math.cos(rot) - y * math.sin(rot), cy + x * math.sin(rot) + y * math.cos(rot)))
                L['items'].append(f"stroke {nk():#x} {clip} {flags} {a} {rgba(o['color']):#010x} {2.6 * k:.3f} 3 "
                                  + ' '.join(f'{x:.2f} {y:.2f}' for x, y in pts))
            elif sym in ICONS:
                size = max(r['w'], r['h']) if sym != '#i-bat' else r['w']
                nom, iid = min(ICONS[sym], key=lambda t: abs(t[0] - size))
                if sym == '#i-power' and '#aIc' in p:
                    iid = 18
                L['items'].append(f"image {nk():#x} {clip} {flags} {a} {rgba(o['color']):#010x} {iid} {r['x']} {r['y']} "
                                  f"{r['w']} {r['h']} 1")
        elif o.get('svg') == '' and p.endswith('> svg') and ('.mk' in p or 'pill' in p or 'shTile' in p or 'lnGlyph' in p) \
                and last_row:
            _, pid, eid = last_row.split(':')
            w = r['w']
            size = min(range(4), key=lambda i: abs((92, 124, 220, 380)[i] - w))
            dim = '.row.ready' not in p and '.row' in p and '#ctxPrev' not in p
            L['items'].append(f"tile {nk():#x} {clip} {flags} {a * (.46 if dim else 1)} k:{eid}:{CAT.get(pid, 0)} {size} "
                              f"{r['x']} {r['y']} {w} {.3 if dim else 1}")
        if p.endswith('.edge'):
            continue
        last = comps[-1]
        if last.startswith('.tag'):
            bg = {'.tag.exp': 0x29ff9f0a, '.tag.stable': 0x2630d158, '.tag.def': 0x336fb3ff}.get(last, 0x1fffffff)
            L['items'].insert(len(L['items']) - 1 if 'text' in o else len(L['items']),
                              f"rect {nk():#x} {clip} {flags} {a} {bg:#010x} {r['x']} {r['y']} {r['w']} {r['h']} {r['h'] / 2} 0")
        if 'bg' in o or ('br' in o and 'glass' not in o and 'sh' in o and 'inset' not in o['sh']):
            br = o.get('br', '0px')
            rad = min(r['w'], r['h']) / 2 if br.endswith('%') else float(br.split('px')[0] or 0)
            if 'sh' in o and 'inset' not in o['sh']:
                m = re.match(r'(rgba?\([^)]*\)) (-?[\d.]+)px (-?[\d.]+)px ([\d.]+)px (-?[\d.]+)px', o['sh'])
                if m:
                    L['items'].append(f"shadow {nk():#x} {clip} {flags} {a} {rgba(m.group(1)):#010x} {r['x']} {r['y']} "
                                      f"{r['w']} {r['h']} {rad} {m.group(2)} {m.group(3)} {m.group(4)} {m.group(5)}")
            if 'bg' in o:
                L['items'].append(f"rect {nk():#x} {clip} {flags} {a} {rgba(o['bg']):#010x} {r['x']} {r['y']} {r['w']} "
                                  f"{r['h']} {rad} 0")
        # rings of status dots (.sd.missing / .sd.coming): borders are not in the dump
        if comps[-1] == '.sd' and 'bg' not in o:
            row = next((c for c in comps if c.startswith('.row')), '')
            if 'coming' in row:
                L['items'].append(f"rect {nk():#x} {clip} {flags | 4} {a} 0xd9ff9f0a {r['x']} {r['y']} {r['w']} {r['h']} "
                                  f"{r['w'] / 2} 3.5")
            elif 'missing' in row:
                L['items'].append(f"rect {nk():#x} {clip} {flags} {a} 0x4debebf5 {r['x']} {r['y']} {r['w']} {r['h']} "
                                  f"{r['w'] / 2} 3.5")
    # the focus lens (lensTarget insets), last in its layer
    if state in FOCUS:
        layer, fk = FOCUS[state]
        el = next((o for o in d if o.get('f') == fk and o.get('a', 1) > .002), None)
        if el:
            r = el['r']
            t = fk[:2]
            ix, iy, rad = 0, 0, r['h'] / 2
            if t == 'p:':
                ix, iy, rad = 16, 16, 80
            elif t == 'r:':
                ix, iy, rad = 16, 8, 62
            elif fk in ('t:power', 'sh:close'):
                ix, iy, rad = 12, 12, (r['h'] - 24) / 2
            elif fk.startswith('m:auto:'):
                rad = r['h'] / 2
            elif t == 'm:':
                ix, iy, rad = 16, 4, 58
            elif fk == 'sh:def':
                ix, iy, rad = 10, 8, 56
            fl = 2 | (1 if t in ('p:', 'r:') else 0)
            lay[layer]['shapes'].append(f"shape {0x1e45:#x} {fl} {r['x'] + ix} {r['y'] + iy} {r['w'] - 2 * ix} "
                                        f"{r['h'] - 2 * iy} {rad} 1 1.4 .75 .85 1 .06")
    out = ['# ' + path, 'frame', 'dim 0.8', f"back {1 if state in BACK1 else 0}"]
    for name in ('base', 'over'):
        L = lay[name]
        out.append('layer ' + name)
        for c in L['clips']:
            out.append('clip %s %s %s %s 0' % c)
        out += L['shapes'][:20] + L['items'][:448]
    print('\n'.join(out))


main()
