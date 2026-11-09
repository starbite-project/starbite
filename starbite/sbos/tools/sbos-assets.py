#!/usr/bin/env python3
"""Starbite OS's own asset sections (docs/STARBITE-OS.md 3.1): two large fonts and the app icons, added to
libsb's container by starbite/sbos/build.sh.

    build/venv/bin/python starbite/sbos/tools/sbos-assets.py [OUT]      (default build/brand/sbos-extra.sb7)

Fonts are rasterised with menu v7's own pipeline (tools/brand/menu7-assets.py: HarfBuzz + FreeType, the
same Inter Tight file, OFL), so they match the menu's type:
  30  Inter Tight 300, 380 px, digits and ':' (tnum): the lock screen's clock
  31  Inter Tight 600, 150 px, digits, A-Z, a-z, a few signs: big values ("38 °C", "72 %", "SOS")
Icons are drawn here as simple SVG line art (24-unit grid, Starbite's own drawings) and rendered with
rsvg-convert to A8 at their nominal sizes: 200.. app tiles (150 px), 220.. small (64 px).
Output: an SB7ASSET container holding only these sections (merged by starbite/sbos/tools/mergeassets.py).
"""
import importlib.util
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
spec = importlib.util.spec_from_file_location('menu7_assets', ROOT / 'tools/brand/menu7-assets.py')
m7 = importlib.util.module_from_spec(spec)
sys.modules['menu7_assets'] = m7
spec.loader.exec_module(m7)

np = m7.np

FONTS = {
    30: (m7.FAM_IT, 300, 380, m7.chars('0123456789:'), m7.FF_SUBSET | m7.FF_TNUM),
    31: (m7.FAM_IT, 600, 150, m7.chars('0123456789:%.-+/ ' + '°' + 'ABCDEFGHIJKLMNOPQRSTUVWXYZ' + 'abcdefghijklmnopqrstuvwxyz'),
         m7.FF_SUBSET | m7.FF_TNUM),
}

# 24-unit line icons (stroke, round caps), Starbite's own drawings
ICONS = {
    'light': '<path d="M7 2.5h10l-1.8 5.2v12.3a1.5 1.5 0 0 1-1.5 1.5h-3.4a1.5 1.5 0 0 1-1.5-1.5V7.7z"/>'
             '<path d="M8.3 7.7h7.4"/><path d="M12 12v3"/>',
    'heat': '<path d="M12 2.5c.6 3.2 5.5 5.6 5.5 11a5.5 5.5 0 0 1-11 0c0-2.9 1.6-4.5 2.4-6.2 1.2 1.1 1.7 2.3 1.7 3.6'
            ' 1.6-1.9 1.9-5 1.4-8.4z"/><path d="M12 20.5a2.3 2.3 0 0 1-2.3-2.3c0-1.4 1.2-2.3 2.3-3.8 1.1 1.5 2.3 2.4 2.3 3.8'
            'a2.3 2.3 0 0 1-2.3 2.3z"/>',
    'sos': '<path d="M6 17.5v-5a6 6 0 0 1 12 0v5"/><path d="M4 17.5h16v3.2H4z"/>'
           '<path d="M12 2v2.2"/><path d="M3.8 5.6l1.6 1.6"/><path d="M20.2 5.6l-1.6 1.6"/><path d="M12 10.5v3.5"/>',
    'clock': '<circle cx="12" cy="12" r="9.2"/><path d="M12 6.8V12l3.6 2.2"/>',
    'battery': '<rect x="2.5" y="7" width="17" height="10" rx="2.2"/><path d="M21.5 10.5v3"/>'
               '<path d="M6 10.2v3.6"/><path d="M9.5 10.2v3.6"/><path d="M13 10.2v3.6"/>',
    'settings': '<path d="M4 6.5h9"/><path d="M17 6.5h3"/><circle cx="15" cy="6.5" r="2"/>'
                '<path d="M4 12h3"/><path d="M11 12h9"/><circle cx="9" cy="12" r="2"/>'
                '<path d="M4 17.5h11"/><path d="M19 17.5h1"/><circle cx="17" cy="17.5" r="2"/>',
    'info': '<rect x="3.5" y="5" width="17" height="14" rx="2.5"/><path d="M9 9.2v5.6"/><path d="M6.2 12h5.6"/>'
            '<path d="M14.5 10h3"/><path d="M14.5 14h3"/>',
    'back': '<path d="M15 4.5L7.5 12l7.5 7.5"/>',
    'power': '<path d="M12 3v8"/><path d="M7.2 6.3a7.5 7.5 0 1 0 9.6 0"/>',
    'moon': '<path d="M19.5 14.5A8 8 0 0 1 9.5 4.5a8 8 0 1 0 10 10z"/>',
    'restart': '<path d="M4.5 12a7.5 7.5 0 1 0 2.2-5.3"/><path d="M4.5 3.5v3.7h3.7"/>',
    'menu': '<rect x="4" y="4" width="6.5" height="6.5" rx="1.5"/><rect x="13.5" y="4" width="6.5" height="6.5" rx="1.5"/>'
            '<rect x="4" y="13.5" width="6.5" height="6.5" rx="1.5"/><rect x="13.5" y="13.5" width="6.5" height="6.5" rx="1.5"/>',
    'lock': '<rect x="5" y="10.5" width="14" height="10" rx="2"/><path d="M8 10.5V7.5a4 4 0 0 1 8 0v3"/>',
    # 0.3.2: Emergency (a handset with a cross), Wi-Fi (for the radio's tile when it comes)
    'emergency': '<path d="M5.6 5.2h2.7l1.4 4.1-2 1.5a11 11 0 0 0 5.6 5.6l1.5-2 4.1 1.4v2.7a2 2 0 0 1-2.2 2'
                 'A15.5 15.5 0 0 1 3.6 7.4a2 2 0 0 1 2-2.2z"/><path d="M18 2.5v5.5"/><path d="M15.25 5.25h5.5"/>',
    'wifi': '<path d="M2.5 9a14 14 0 0 1 19 0"/><path d="M5.8 12.6a9.2 9.2 0 0 1 12.4 0"/>'
            '<path d="M9.1 16.1a4.4 4.4 0 0 1 5.8 0"/><circle cx="12" cy="19.4" r=".6"/>',
    # 1.2-alpha20: Calculator (a body, a display, keys), Notes (a page with lines and a folded corner)
    'calc': '<rect x="5" y="2.5" width="14" height="19" rx="2.2"/><rect x="8" y="5.5" width="8" height="3.5" rx=".8"/>'
            '<path d="M8.5 13h.01"/><path d="M12 13h.01"/><path d="M15.5 13h.01"/>'
            '<path d="M8.5 17h.01"/><path d="M12 17h.01"/><path d="M15.5 17h.01"/>',
    'notes': '<path d="M5.5 2.5h9l4 4v13a2 2 0 0 1-2 2h-11a2 2 0 0 1-2-2v-15a2 2 0 0 1 2-2z"/>'
             '<path d="M14.5 2.5v4h4"/><path d="M8 11h8"/><path d="M8 14.5h8"/><path d="M8 18h5"/>',
}
# id: (icon, px, stroke in 24-unit) -- ids must match starbite/sbos/kern/ui.c ICON_*
ICON_IDS = {
    200: ('light', 150, 1.6), 201: ('heat', 150, 1.6), 202: ('sos', 150, 1.6), 203: ('clock', 150, 1.6),
    204: ('battery', 150, 1.6), 205: ('settings', 150, 1.6), 206: ('info', 150, 1.6),
    220: ('back', 64, 2.4), 221: ('power', 96, 2.0), 222: ('moon', 96, 2.0), 223: ('restart', 96, 2.0),
    224: ('menu', 96, 2.0), 225: ('lock', 64, 2.2), 226: ('light', 96, 2.0), 227: ('sos', 96, 2.0),
    207: ('emergency', 150, 1.6), 208: ('wifi', 150, 1.6), 228: ('emergency', 96, 2.0),
    209: ('calc', 150, 1.6), 210: ('notes', 150, 1.6),
}


def icon(name, size, sw):
    nw = nh = size
    spill = sw * size / 24 / 2
    mg = int(np.ceil(spill + 2))
    W, H = nw + 2 * mg, nh + 2 * mg
    inner = (f'<svg x="{mg}" y="{mg}" width="{nw}" height="{nh}" viewBox="0 0 24 24" overflow="visible" '
             f'fill="none" stroke="#fff" stroke-width="{sw}" stroke-linecap="round" stroke-linejoin="round">'
             f'{ICONS[name]}</svg>')
    rgba = m7.rsvg(m7.svg_doc(W, H, inner, '<defs/>'), W, H)
    a = np.clip(np.rint(rgba[..., 3]), 0, 255).astype(np.uint8)
    return m7.image_section(a, m7.A8, (mg, mg, nw, nh))


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'build/brand/sbos-extra.sb7'
    secs = {}
    m7.FONT_SPEC.update(FONTS)
    for fid in FONTS:
        body, info = m7.build_font(fid)
        if info['missing']:
            print(f'font {fid}: missing {info["missing"]!r}')
        secs[(m7.T_FONT, fid)] = body
        print(f'font {fid}: {info["glyphs"]} glyphs, {info["kern"]} kerning pairs, {info["bytes"]} bytes')
    for iid, (name, px, sw) in ICON_IDS.items():
        secs[(m7.T_IMAGE, iid)] = icon(name, px, sw)
    blob = m7.container(secs)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(blob)
    print(f'{out}: {len(secs)} sections, {len(blob)} bytes')


if __name__ == '__main__':
    main()
