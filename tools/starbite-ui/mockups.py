#!/usr/bin/env python3
"""Starbite OS UI mockups (2026-11-10): display lists drawn by the boot menu's own renderer (h7 --list: the real
glass, the menu's wallpaper, fonts, OS tiles and provider glyphs), so the lock screen and home look exactly like the
boot menu. Design only: nothing here runs on the phone.

    python3 -I tools/starbite-ui/mockups.py <scene> > scene.dl       scenes: lock expanded swipe home emergency
    H7_SRC=<this checkout> H7_OUT=<dir> sh tools/menu7/preview/run.sh list scene.dl out.png
"""
import sys

W, H = 1440, 2960
L1, L2, L3 = 0xffffffff, 0x9eebebf5, 0x57ebebf5
GREEN = 0xff30d158
RED = 0xffff453a
PANE = (.02, .035, .08, .22)                 # M7_TINT_PANE
TINT = {                                     # provider tile tints (menu7.h M7_TINT_*), approximated
    'apple': (.55, .57, .62, .30), 'microsoft': (.10, .32, .78, .34), 'android': (.13, .55, .30, .32),
    'linux': (.78, .52, .12, .34), 'starbite': (.35, .55, .95, .34), 'lab': (.55, .30, .85, .34),
}
GLYPH = {'apple': 64, 'microsoft': 65, 'android': 66, 'linux': 67, 'starbite': 68, 'lab': 69}
CAT = {'apple': 0, 'microsoft': 1, 'android': 2, 'linux': 3, 'starbite': 4, 'lab': 6}
F = dict(IT700_100=3, IT700_92=4, IT700_70=5, IT600_66=7, IT600_54=8, IT600_46=9, IT300_56=12,
         IN600_58=14, IN600_46=16, IN600_33=17, IN400_48=19, IN400_44=21, IN400_42=22, IN400_40=23,
         IN400_38=24, IN400_36=25)
# what the boot menu lists (tools/store/catalog.toml's visible entries, its order and names); Starbite OS itself
# is the running system, so its group shows the other Starbite systems
GROUPS = [
    ('apple', 'Apple', [('ios16', 'iOS 16', '16.5 (20F66) · an earlier port kernel'), ('mac13', 'macOS Ventura', '13.5 · root on microSD'),
                        ('ios26', 'iOS 26', '26.6.2 · XNU 12377')]),
    ('microsoft', 'Microsoft', [('win11', 'Windows 11', '26100 · UEFI')]),
    ('android', 'Google', [('android', 'One UI 8', 'Android 16'), ('chromeos', 'ChromiumOS', 'Flex')]),
    ('linux', 'Linux', [('linux-ubuntu', 'Ubuntu', '26.04 LTS · Phosh'), ('linux-pmos', 'postmarketOS', 'edge')]),
    ('starbite', 'Starbite', [('engineer', 'Engineer OS', '2.0'), ('utility', 'Utility OS', '2.0')]),
]


class DL:
    def __init__(self):
        self.out = ['frame full', 'dim 0.8', 'back 0']
        self.k = 0x100
        self.layer = None
        self.clips = 0

    def key(self):
        self.k += 1
        return self.k

    def set_layer(self, which):
        if self.layer != which:
            self.out.append(f'layer {which}')
            self.layer = which
            self.clips = 0

    def clip(self, x0, y0, x1, y1, r=0):
        self.out.append(f'clip {x0} {y0} {x1} {y1} {r}')
        self.clips += 1
        return self.clips

    def shape(self, x, y, w, h, r, tint=PANE, a=1.0, s=1.0, flags=0):
        self.out.append(f'shape 0x{self.key():x} {flags} {x} {y} {w} {h} {r} {a} {s} {tint[0]} {tint[1]} {tint[2]} {tint[3]}')

    def text(self, s, x, y, font, color=L1, track=0.0, scale=1.0, max_w=0, a=1.0, clip=0, flags=0):
        self.out.append(f'text 0x{self.key():x} {clip} {flags} {a} 0x{color:08x} {F[font]} {x} {y} {track} {scale} {max_w} 0 0 0 0 |{s}')

    def image(self, gid, x, y, w, h, color=L1, a=1.0, clip=0):
        self.out.append(f'image 0x{self.key():x} {clip} 0 {a} 0x{color:08x} {gid} {x} {y} {w} {h} 1')

    def tile(self, key, cat, x, y, w, size=1, a=1.0, clip=0):
        self.out.append(f'tile 0x{self.key():x} {clip} 0 {a} k:{key}:{cat} {size} {x} {y} {w} 1')

    def rect(self, x, y, w, h, r, color, a=1.0, ring=0, clip=0):
        self.out.append(f'rect 0x{self.key():x} {clip} 0 {a} 0x{color:08x} {x} {y} {w} {h} {r} {ring}')

    def stroke(self, width, pts, color=L1, a=1.0):
        for i in range(len(pts) - 1):      # one segment per item (2 points each)
            (x0, y0), (x1, y1) = pts[i], pts[i + 1]
            self.out.append(f'stroke 0x{self.key():x} 0 0 {a} 0x{color:08x} {width} 2 {x0} {y0} {x1} {y1}')

    def arc(self, cx, cy, r, width, a0, sweep, color, a=1.0):
        self.out.append(f'arc 0x{self.key():x} 0 0 {a} 0x{color:08x} {cx} {cy} {r} {width} {a0} {sweep}')

    def dump(self):
        return '\n'.join(self.out) + '\n'


def tw(s, size):
    """an estimate of a text's width (Inter at ~.53 em a character): for centring only"""
    return int(len(s) * size * .53)


def status_bar(d, dy=0, a=1.0):
    d.text('Galaxy S9', 76, 129 + dy, 'IT600_46', track=-.01, a=a)
    d.text('87%', 1186, 129 + dy, 'IT600_46', track=-.01, a=a)
    d.image(23, 1282, 96 + dy, 84, 40, a=a)


def lock_top(d, dy=0, a=1.0):
    # the boot menu's wordmark, then the date and the clock: left-aligned like "Choose a System"
    d.text('starbite', 78, 262 + dy, 'IT300_56', color=L2, track=-.02, a=a)
    d.image(24, 276, 222 + dy, 24, 24, color=0xff6fb3ff, a=a)
    d.text('Tuesday, 10 November', 80, 372 + dy, 'IT600_46', color=L2, track=-.01, a=a)
    d.text('21:47', 62, 640 + dy, 'IT700_100', track=-.03, scale=2.75, a=a)


def card(d, x, y, w, prov, title, names, meta, dy=0, a=1.0, h=210):
    d.shape(x, y + dy, w, h, 72, a=a)
    d.shape(x + 34, y + 33 + dy, 144, 144, 40, tint=TINT[prov], s=.9, a=a)
    d.image(GLYPH[prov], x + 64, y + 63 + dy, 84, 84, a=a)
    d.text(title, x + 212, y + 96 + dy, 'IT600_66', track=-.02, a=a)
    d.text(meta, x + w - 40 - tw(meta, 38), y + 92 + dy, 'IN400_38', color=L3, a=a)
    d.rect(x + 214, y + 140 + dy, 20, 20, 10, GREEN, a=a)
    d.text(names, x + 248, y + 160 + dy, 'IN400_44', color=L2, max_w=w - 300, flags=0, a=a)


def lock_cards(d, dy=0, a=1.0, expand=None):
    y = 790
    for prov, title, rows in GROUPS:
        names = ', '.join(r[1] for r in rows)
        meta = f'{len(rows)} system' + ('s' if len(rows) > 1 else '')
        if prov == expand:
            h = 210 + len(rows) * 150 + 40
            d.shape(40, y + dy, 1360, h, 72, a=a)
            d.shape(74, y + 33 + dy, 144, 144, 40, tint=TINT[prov], s=.9, a=a)
            d.image(GLYPH[prov], 104, y + 63 + dy, 84, 84, a=a)
            d.text(title, 252, y + 96 + dy, 'IT600_66', track=-.02, a=a)
            d.text('Tap a system, then slide to start', 252, y + 160 + dy, 'IN400_44', color=L2, a=a)
            ry = y + 220
            for i, (key, name, detail) in enumerate(rows):
                sel = i == len(rows) - 1
                if sel:   # the chosen one: its own glass row with the slide track
                    d.shape(70, ry + dy - 6, 1300, 140, 60, tint=(.10, .16, .30, .30), a=a)
                d.tile(key, CAT[prov], 96, ry + 12 + dy, 112, size=1, a=a)
                d.text(name, 240, ry + 66 + dy, 'IT600_54', track=-.015, a=a)
                d.text(detail, 240, ry + 112 + dy, 'IN400_40', color=L2, a=a)
                ry += 150
            y += h + 24
            # the slide-to-start track under the card (iOS "slide to power off"): a pill and a knob
            d.shape(40, y + dy, 1360, 150, 75, tint=(.04, .06, .12, .30), a=a)
            d.rect(58, y + 18 + dy, 114, 114, 57, L1, a=a)
            d.image(7, 86, y + 46 + dy, 58, 58, color=0xff0b1630, a=a)
            d.text('slide to start iOS 26', 470, y + 92 + dy, 'IN400_48', color=L2, a=a)
            d.text('›››', 1250, y + 92 + dy, 'IN400_48', color=L3, a=a)
            y += 150 + 24
            continue
        card(d, 40, y, 1360, prov, title, names, meta, dy=dy, a=a)
        y += 234


def lock_bottom(d, dy=0, a=1.0):
    # iOS's lock-screen corners: the light (left) and Emergency (right), round glass, press and hold
    d.shape(84, 2620 + dy, 150, 150, 75, tint=(.06, .08, .16, .30), a=a)
    d.stroke(5, [(134, 2662 + dy), (184, 2662 + dy), (174, 2692 + dy), (144, 2692 + dy), (134, 2662 + dy)], a=a)
    d.rect(146, 2694 + dy, 26, 50, 7, L1, a=a, ring=5)   # a torch: an outline head and body, a switch
    d.rect(155, 2708 + dy, 8, 12, 3, L1, a=a)
    d.shape(1206, 2620 + dy, 150, 150, 75, tint=(.45, .05, .05, .40), a=a)
    d.text('SOS', 1236, 2715 + dy, 'IT600_46', color=L1, track=.02, a=a)
    d.text('Swipe up to open', W // 2 - tw('Swipe up to open', 38) // 2, 2858 + dy, 'IN400_38', color=L3, a=a)
    d.rect(W // 2 - 210, 2900 + dy, 420, 14, 7, L1, a=.85 * a)   # the home indicator


def scene_lock(expand=None):
    d = DL()
    d.set_layer('base')
    status_bar(d)
    lock_top(d)
    lock_cards(d, expand=expand)
    lock_bottom(d)
    return d.dump()


APPS = [  # name, tint, glyph (a line icon id, or None for a short label)
    ('Light', (.95, .78, .10, .36), None, 'Aa'), ('Clock', (.20, .40, .85, .34), 13, None),
    ('Notes', (.95, .80, .20, .34), None, 'N'), ('Files', (.20, .55, .90, .34), 15, None),
    ('Calculator', (.55, .30, .85, .34), None, '+-'), ('Wi-Fi', (.10, .45, .95, .34), None, 'W'),
    ('Battery', (.15, .65, .35, .34), 23, None), ('Settings', (.45, .48, .55, .34), 8, None),
    ('Modem', (.20, .60, .75, .34), None, 'M'), ('Info', (.85, .25, .25, .34), 9, None),
]


DOCK = [('Clock', (.20, .40, .85, .34), 13, None), ('Notes', (.95, .80, .20, .34), None, 'N'),
        ('Files', (.20, .55, .90, .34), 15, None), ('Settings', (.45, .48, .55, .34), 8, None)]


def home(d, dy=0, a=1.0, widget=True):
    if widget:                              # the status bar and the clock widget (a frosted pane, as the menu's cards)
        status_bar(d, dy, a)
        d.shape(40, 190 + dy, 1360, 330, 72, a=a)
        d.text('21:47', 96, 400 + dy, 'IT700_100', track=-.03, scale=1.5, a=a)
        d.text('Tuesday, 10 November', 100, 470 + dy, 'IN400_44', color=L2, a=a)
        d.text('87%', 1110, 340 + dy, 'IT600_66', a=a)
        d.text('about 9 h', 1110, 410 + dy, 'IN400_40', color=L2, a=a)
    # the grid: 4 columns of 236 px glass squircles (the boot menu's tile glass), names under them
    x0, ys, cw = 76, 620, 340
    for i, (name, tint, icon, label) in enumerate([x for x in APPS if x[0] not in [y[0] for y in DOCK]]):
        cx = x0 + (i % 4) * cw
        cy = ys + (i // 4) * 380
        d.shape(cx + 20, cy + dy, 236, 236, 64, tint=tint, s=.9, a=a)
        if icon is not None:
            sz = 96 if icon != 23 else 110
            hh = 96 if icon != 23 else 52
            d.image(icon, cx + 20 + (236 - sz) // 2, cy + dy + (236 - hh) // 2, sz, hh, a=a)
        else:
            d.text(label, cx + 20 + 118 - tw(label, 92) // 2, cy + dy + 150, 'IT700_92', a=a)
        d.text(name, cx + 138 - tw(name, 40) // 2, cy + 296 + dy, 'IN400_40', a=a)
    # the dock: a pill of glass with the systems and the phone's own apps
    d.shape(40, 2560 + dy, 1360, 300, 96, a=a)
    for i, (name, tint, icon, label) in enumerate(DOCK):
        cx = 96 + i * 330
        d.shape(cx, 2592 + dy, 236, 236, 64, tint=tint, s=.9, a=a)
        if icon is not None:
            d.image(icon, cx + 70, 2592 + dy + 70, 96, 96, a=a)
        else:
            d.text(label, cx + 118 - tw(label, 92) // 2, 2592 + dy + 150, 'IT700_92', a=a)
    d.rect(W // 2 - 210, 2900 + dy, 420, 14, 7, L1, a=.85 * a)


def scene_home():
    d = DL()
    d.set_layer('base')
    home(d)
    return d.dump()


def scene_swipe():
    # mid-gesture: the finger 1150 px up from the bar; the lock screen follows it (the clock already gone), the home's
    # apps rise into place from below, still faint; the status bar stays put
    d = DL()
    d.set_layer('base')
    status_bar(d)
    home(d, dy=420, a=.30, widget=False)
    lock_cards(d, dy=-1150, a=.92)          # the corners fade out as soon as the finger moves (as on iOS)
    d.rect(W // 2 - 210, 2900, 420, 14, 7, L1, a=.85)
    return d.dump()


def scene_emergency():
    # swiped left from the lock screen (as iOS's camera): the Emergency page; the normal phone stays safe and simple
    d = DL()
    d.out.append('dim 0.92')
    d.set_layer('base')
    status_bar(d)
    d.text('starbite', 78, 262, 'IT300_56', color=L2, track=-.02)
    d.image(24, 300, 220, 24, 24, color=0xffff6b5e)
    d.text('Emergency', 74, 420, 'IT700_92', track=-.03)
    d.text('Help, light, heat and the battery, one tap each', 80, 500, 'IN400_44', color=L2)
    tiles = [('SOS', 'Call and flash for help', (.60, .06, .06, .42)), ('Light', 'Torch and screen light', (.70, .55, .08, .36)),
             ('Heat', 'Warm your hands', (.85, .35, .06, .38)), ('Survival', 'The most heat, to 2 %', (.75, .10, .10, .40)),
             ('Medical ID', 'For whoever finds you', (.85, .20, .30, .36)), ('Save power', 'Days on one charge', (.10, .55, .30, .34))]
    y = 600
    for i, (name, line, tint) in enumerate(tiles):
        x = 40 + (i % 2) * 690
        yy = y + (i // 2) * 470
        d.shape(x, yy, 670, 440, 80, tint=tint, s=.95)
        d.text(name, x + 50, yy + 300, 'IT700_70', track=-.02)
        d.text(line, x + 52, yy + 370, 'IN400_40', color=L2, max_w=580)
    d.shape(40, 2040, 1360, 210, 72)
    d.text('Battery 87% · about 9 h', 96, 2130, 'IT600_54')
    d.text('Saving power: the screen dims, Wi-Fi stays off', 98, 2196, 'IN400_38', color=L2, max_w=1260)
    d.text('Swipe right to go back', W // 2 - tw('Swipe right to go back', 38) // 2, 2858, 'IN400_38', color=L3)
    d.rect(W // 2 - 210, 2900, 420, 14, 7, L1, a=.85)
    return d.dump()


SCENES = {'lock': lambda: scene_lock(), 'expanded': lambda: scene_lock('apple'), 'swipe': scene_swipe,
          'home': scene_home, 'emergency': scene_emergency}

if __name__ == '__main__':
    sys.stdout.write(SCENES[sys.argv[1] if len(sys.argv) > 1 else 'lock']())
