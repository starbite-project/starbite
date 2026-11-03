#!/usr/bin/env python3
"""Display lists that show every OS tile of tools/store/catalog.toml and every provider glyph as
glass draws them (h7 --list), for checking the logos (tools/brand/menu7_logos.py).

    python3 tools/menu7/preview/logos-dl.py small > a.dl     tiles at 92, 124, 220 + glyphs
    python3 tools/menu7/preview/logos-dl.py large > b.dl     tiles at 380
    sh tools/menu7/preview/run.sh list a.dl out.png
"""
import sys

CAT = {'apple': 0, 'microsoft': 1, 'android': 2, 'linux': 3, 'starbite': 4, 'other': 5, 'lab': 6}
KEYS = [('ios26', 'apple'), ('win11', 'microsoft'), ('android', 'android'),
        ('linux-ubuntu', 'linux'), ('linux-ubuntu-gnome', 'linux'), ('linux-ubuntu-lomiri', 'linux'),
        ('linux-arch', 'linux'), ('linux-pmos', 'linux'), ('linux-kali', 'linux'),
        ('linux-debian', 'linux'), ('linux-fedora', 'linux'), ('linux-shell', 'linux'),
        ('@lab', 'lab'), ('lab-failtest', 'lab'),
        ('?', 'apple'), ('?', 'microsoft'), ('?', 'android'), ('?', 'linux')]
SIZES = {92: 0, 124: 1, 220: 2, 380: 3}


def tiles(px, y0, cols, gap, out, key0):
    k = key0
    for i, (key, cat) in enumerate(KEYS):
        x = 60 + (i % cols) * (px + gap)
        y = y0 + (i // cols) * (px + gap)
        out.append(f'tile 0x{k:x} 0 0 1 k:{key}:{CAT[cat]} {SIZES[px]} {x} {y} {px} 1')
        k += 1
    return y0 + ((len(KEYS) + cols - 1) // cols) * (px + gap), k


def main():
    which = sys.argv[1] if len(sys.argv) > 1 else 'small'
    out = ['frame full', 'dim 0.8', 'back 0', 'layer base', 'layer over']
    if which == 'small':
        y, k = tiles(92, 60, 12, 18, out, 0x100)
        y, k = tiles(124, y + 30, 9, 26, out, k)
        y, k = tiles(220, y + 30, 5, 40, out, k)
        # provider glyphs 64..70 at their 96 px box, on a provider-tile-sized dark plate
        for i, gid in enumerate(range(64, 71)):
            x = 60 + i * 190
            out.append(f'rect 0x{k:x} 0 0 1 0xff1c2233 {x - 38} {y + 10} 172 172 46 0')
            out.append(f'image 0x{k + 1:x} 0 0 1 0xffffffff {gid} {x} {y + 48} 96 96 1')
            k += 2
    else:
        tiles(380, 60, 3, 70, out, 0x100)
    print('\n'.join(out))


if __name__ == '__main__':
    main()
