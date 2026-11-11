#!/usr/bin/env python3
"""The real OS and company logos for menu v7 (provider glyphs and OS tile glyph layers).

    build/venv/bin/python tools/brand/menu7_logos.py --fetch     download every source logo again
                                                                 and check it against the copy in
                                                                 tools/brand/logos/src/ (sha256)
    build/venv/bin/python tools/brand/menu7_logos.py --sheet OUT.png   every logo, as rendered

Used by tools/brand/menu7-assets.py. Sources, licences and trademark notes:
tools/brand/logos/SOURCES.md.

The source SVGs are committed under tools/brand/logos/src/ (about 120 KB), so the build does not
depend on the source sites: Commons serves a file's current revision at its URL and kali.org's
file can change. --fetch downloads each one into a temporary file and fails (exit 1, nothing kept)
on a sha256 mismatch, so a changed upstream file is noticed and never replaces the pinned copy.

The SVGs are treated as data: never executed or included in anything; each one is checked
against its sha256 and for external references, edited only by removing whole elements named
here (a background square, a wordmark), then rasterised by rsvg-convert straight to the target
pixel size (no resampling), and cropped to the logo's own bounds measured from a 2048 px render.
rsvg-convert writes straight (unpremultiplied) alpha; menu7-assets premultiplies once when it
stores the pixels, so fully transparent pixels are 0x00000000.
"""
import argparse
import hashlib
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'tools/brand/logos/src'
COMMONS = 'https://upload.wikimedia.org/wikipedia/commons/'

# name: (directory/file under build/logos-src, URL, sha256)
FILES = {
    'apple': ('apple/Apple_logo_black.svg', COMMONS + 'f/fa/Apple_logo_black.svg',
              '9d00ea77a3240f291356c36261c5f45d7fa456f29c97a2f060caf0ed4b9c3231'),
    'windows11': ('windows11/Windows_logo_-_2021.svg', COMMONS + '8/87/Windows_logo_-_2021.svg',
                  '0f6b74d8d8ede1ec0e454b3c6fcdfba65fbe989af9a49ad4ff0c498f4dc3afbb'),
    'microsoft': ('microsoft/Microsoft_logo.svg', COMMONS + '4/44/Microsoft_logo.svg',
                  '3d41251f93127b4b42c2f69fa423d204946cf9c307d786ea36b8d9bef4179282'),
    'google-g': ('google-g/Google__22G_22_logo.svg', COMMONS + 'c/c1/Google_%22G%22_logo.svg',
                 'ed9087d76cdc6d1c53698f6068f79872e77e87c8d012c0cfdad13b05b6ccb37c'),
    'android': ('android-head/Android_robot_head.svg', COMMONS + '3/31/Android_robot_head.svg',
                '0d3d8e90f448472d65eafefc7aaf4b6836517d840b96e0613ca7078879c279f2'),
    'ubuntu': ('ubuntu-2022/Ubuntu-logo-no-wordmark-2022.svg',
               COMMONS + '0/00/Ubuntu-logo-no-wordmark-2022.svg',
               '808267226514c1ad3a75269de5b6fd2ecf1bc362fb1a244c57d85c798a823ce9'),
    'arch': ('arch-official/archlinux-logo-dark-scalable.svg',
             'https://archlinux.org/static/logos/archlinux-logo-dark-scalable.svg',
             'a7f37c9c87f0e0e37454b47ec1221b61416aa9e319cc46a1f0b5b70c4a1dcb6e'),
    'kali': ('kali/kali-dragon-icon.svg', 'https://www.kali.org/images/kali-dragon-icon.svg',
             '9a3ed29a936b48cffe54795f9cabfd351a79dfcf861024db200fb569e6fcc2a6'),
    # the project's artwork repository at the commit that added the logo (2017-08-26)
    'pmos': ('pmos/logo.svg', 'https://gitlab.com/postmarketOS/artwork/-/raw/'
             'ede3978936131a44e121db1d4e642af405a2a8e9/logo/logo.svg',
             'a4162f754b853293fbe2be804466c16406765f2cf6959a0447b5c90ce588b4b5'),
    'tux': ('tux/Tux.svg', COMMONS + '3/35/Tux.svg',
            'cd503ad510e16ff2869f959cf57b892bb2175a6874ff696b495bd94fd7db9743'),
    'debian': ('debian/Debian-OpenLogo.svg', COMMONS + '4/4a/Debian-OpenLogo.svg',
               'a759c381560e82e8d719e0038d0d95209b85c00b0d0f6b92c5fc510b959226b7'),
    'fedora': ('fedora/Fedora_icon__282021_29.svg', COMMONS + '4/41/Fedora_icon_%282021%29.svg',
               'e0cad9aa4736a492cbeed7f875cb6a7ddfeab54879fc9e218fc00933ef18b6b6'),
    'chromium': ('chromium/Chromium_Material_Icon.svg', COMMONS + 'f/fe/Chromium_Material_Icon.svg',
                 '72a4e02e7032106bc04e21c4133b42dde6903d6b8b7c3ef8164967a945883c54'),
}

# Anything that could load or run something: scripts, foreign content, images, and any href or
# url() (either quote, or none) that is not a local '#id' reference.
BAD = re.compile(r'<script|foreignObject|<image|href\s*=(?!\s*[\'"]\s*#)|url\((?!\s*[\'"]?\s*#)'
                 r'|@import', re.I)

# Short credits for the INFO section of menu7.sb7 (ASCII), so the notice travels with the built
# container and ramdisk; the full notes are in tools/brand/logos/SOURCES.md.
CREDITS = {
    'apple': 'Apple logo: Wikimedia Commons, public domain; trademark of Apple Inc.',
    'windows11': 'Windows 11 logo: Wikimedia Commons, public domain; trademark of Microsoft Corp.',
    'microsoft': 'Microsoft logo: Wikimedia Commons, public domain; trademark of Microsoft Corp.',
    'google-g': 'Google G logo: Wikimedia Commons, public domain; trademark of Google LLC',
    'android': 'Android robot: Google, CC BY 3.0 (Wikimedia Commons); Android is a trademark of '
               'Google LLC',
    'ubuntu': 'Ubuntu Circle of Friends (2022): Canonical Ltd., GPLv3 (Wikimedia Commons); '
              'trademark of Canonical Ltd.',
    'arch': 'Arch Linux logo: archlinux.org; trademark of the Arch Linux project',
    'kali': 'Kali Linux dragon: kali.org; Kali Linux and the dragon are trademarks of OffSec '
            'Services Ltd.',
    'pmos': 'postmarketOS logo: postmarketOS developers, CC BY-SA 3.0 '
            '(gitlab.com/postmarketOS/artwork); modified: rasterised',
    'tux': 'Tux: Larry Ewing (lewing@isc.tamu.edu), Simon Budig, Garrett LeSage, made with The GIMP',
    'debian': 'Debian Open Use Logo: (c) Software in the Public Interest, Inc., CC BY-SA 3.0; '
              'modified: wordmark removed, rasterised',
    'fedora': 'Fedora logo: Wikimedia Commons, public domain; trademark of Red Hat, Inc.',
    'chromium': 'Chromium logo: Wikimedia Commons, CC BY 2.5; Chromium is a trademark of Google LLC',
}
assert set(CREDITS) == set(FILES)

# Release icons (one per catalog key: the mark of that release, e.g. Apple's "16" for iOS 16),
# optional: a tile uses its release icon only when the file is in src/release/<key>/ and pinned
# here; otherwise it keeps its vendor-logo tile (menu7-assets tile_table). Each value:
#   (file under src/, source URL, sha256, credit line, mode)
#   mode 'bleed': the icon is itself an app icon (a squircle or square with its own background):
#                 it fills the tile, cut to the tile's squircle; a number: a mark (or a round
#                 badge) at that fraction of the tile, centred on the tile's colours.
# A file present but not pinned (sha256 None) stops the build with the sha256 to pin. SVG or PNG
# (PNG: the largest size published, resampled to each tile size with Lanczos). Added 2026-11-05
# (downloads approved by the user; SOURCES.md has the rows).
DEV_ICONS = 'https://developer.apple.com/assets/elements/icons/'
RELEASE = {
    'ios16': ('release/ios16/ios-16-num-256x256_2x.png', DEV_ICONS + 'ios-16-num/ios-16-num-256x256_2x.png',
              '8bb8a4512731a939c86eef1b20bd68ec7fea930e3c9497569b2ed4320e8c3616',
              "iOS 16 icon: Apple Inc. (developer.apple.com); Apple's copyright and trademark", 'bleed'),
    'ios26': ('release/ios26/ios-ipados-26-256x256_2x.png', DEV_ICONS + 'ios-ipados-26/ios-ipados-26-256x256_2x.png',
              '2799c661d348a612dac1ad197fb43b5f9f2b2ab9e840d521e83102a103f1a9b7',
              "iOS 26 icon (the 26 numeral): Apple Inc. (developer.apple.com); Apple's copyright and trademark", 'bleed'),
    'ios27': ('release/ios27/ios-27-num-256x256_2x.png', DEV_ICONS + 'ios-27-num/ios-27-num-256x256_2x.png',
              '7ad3745fa4b3d25cb06552379023e1e5615c84c0d34acac7bb2765939e1e8770',
              "iOS 27 icon (the 27 numeral): Apple Inc. (developer.apple.com); Apple's copyright and trademark", 'bleed'),
    # the macOS Ventura icon (the round wallpaper disc) from Apple's Mac User Guide for macOS 13
    # (support.apple.com/guide/mac-help/welcome/13.0/mac, its landing art "AppLanding_macOS")
    'mac13': ('release/mac13/macos-ventura-userguide-400.png',
              'https://help.apple.com/assets/63BCA927AAE78C58DD7FBE35/63BCA92EAAE78C58DD7FBE76/en_US/'
              'ba4de10ea93536361680a5dea7896f66.png',
              'd23ece9cc256e64b46115b1cf47062c9df653c357b1fcd1c0ac42f4fcd5dda23',
              "macOS Ventura icon: Apple Inc. (Mac User Guide for macOS 13); Apple's copyright and "
              'trademark', .86),
    # the Android 17 badge from the stable release post (on white: _matte_white below)
    'android': ('release/android/metadata-static.png',
                'https://blogger.googleusercontent.com/img/b/R29vZ2xl/AVvXsEgV7zuuXjulHty999mGDWY1kfL8Q9SXjY'
                'YWn-7JTpMfVdNP78eb5fW9shOpvVdEqK0WnNp7AhdO0qc7pXAaqcfTwXgOGsfZyqcQv8wyD-9niWBpZuP6ZAPHBSetW'
                'enN2lMlRS5wi2d71-n8RCYqrLsFhUCEvM7KeoGLnNaDbiyOZQ0vvyr0O580nXK4Vas/s2048/Metadata%20-%20Static.png',
                '637c790be0ce209bee4b24301dddc66b7f129ce902a38f2136154daba5ce1338',
                'Android 17 badge: Google LLC (Android Developers Blog); Android is a trademark of Google LLC',
                .82),
    'linux-ubuntu': ('release/linux-ubuntu/ubuntu-mascot-light.svg',
                     'https://www.omgubuntu.co.uk/wp-content/uploads/2026/02/ubuntu-mascot-light.svg',
                     'b89ff17308d8a6e8bcaff394e6ad740eae3d7d2fa9be2d751966e64deacbcad9',
                     "Ubuntu 26.04 'Resolute Raccoon' mascot: Canonical Ltd. (via OMG! Ubuntu); Ubuntu "
                     'is a trademark of Canonical Ltd.', .74),
}
RELEASE_DIR = SRC / 'release'


def release_file(key):
    """The release icon's path for a catalog key, or None (no file: keep the vendor tile)."""
    rel, url, sha, credit, mode = RELEASE.get(key, (None,) * 5)
    d = RELEASE_DIR / key
    if rel is None:
        found = sorted(p for p in d.glob('*') if p.suffix.lower() in ('.svg', '.png')) if d.is_dir() else []
        if not found:
            return None
        sys.exit(f'menu7_logos: {found[0]} is not pinned: add it to RELEASE[{key!r}] '
                 f'(file {found[0].relative_to(SRC)}, sha256 '
                 f'{hashlib.sha256(found[0].read_bytes()).hexdigest()}, its URL and credit)')
    p = SRC / rel
    if not p.exists():
        return None
    h = hashlib.sha256(p.read_bytes()).hexdigest()
    assert h == sha, f'{p}: sha256 {h}, expected {sha}'
    if p.suffix.lower() == '.svg':
        assert not BAD.search(p.read_text(errors='replace')), f'{p}: external reference or script'
    return p


def _matte_white(im, edge=224.0):
    """A badge on an opaque white page -> the badge on transparency. Only the white outside it
    goes: the near-white pixels connected to the image border (flood fill), plus a 2 px band
    along the rim, are unmixed from white (alpha = distance from white / the rim colour's
    distance `edge`, colour = 255 - (255 - c) / alpha). White inside the badge (the "ANDROID"
    and "17" lettering) is not connected to the border and stays opaque."""
    from PIL import ImageDraw
    rgb = np.asarray(im.convert('RGB')).astype(np.float64)
    d = 255 - rgb.min(axis=2)                           # 0 = white
    light = Image.fromarray(np.where(d < 100, 255, 0).astype(np.uint8), 'L').copy()  # writable
    H, W = d.shape
    border = [(x, 0) for x in range(W)] + [(x, H - 1) for x in range(W)] + \
             [(0, y) for y in range(H)] + [(W - 1, y) for y in range(H)]
    for xy in border:
        if light.getpixel(xy) == 255:
            ImageDraw.floodfill(light, xy, 128)
    out = np.asarray(light) == 128                      # the white outside, as one region
    band = out.copy()
    for _ in range(2):                                  # the anti-aliased rim next to it
        g = band.copy()
        g[1:] |= band[:-1]
        g[:-1] |= band[1:]
        g[:, 1:] |= band[:, :-1]
        g[:, :-1] |= band[:, 1:]
        band = g
    a = np.where(band, np.clip(d / edge, 0, 1), 1.0)
    c = np.where(band[..., None] & (a[..., None] > 0),
                 255 - (255 - rgb) / np.maximum(a[..., None], 1e-6), rgb)
    rgba = np.concatenate([np.clip(c, 0, 255), a[..., None] * 255], axis=-1)
    return Image.fromarray(np.rint(rgba).astype(np.uint8), 'RGBA')


RELEASE_PREP = {'android': _matte_white}    # applied to the pinned source on every build
_PREP_CACHE = {}


def release_render(key, T):
    """The release icon at T x T, straight RGBA float 0..255, and its mode ('bleed' / 'fit')."""
    p = release_file(key)
    mode = RELEASE[key][4]
    if p.suffix.lower() == '.svg':
        a = _rsvg(p.read_text(), ['-w', '1024', '-h', '1024', '--keep-aspect-ratio'])
        im = Image.fromarray(np.clip(np.rint(a), 0, 255).astype(np.uint8), 'RGBA')
    else:
        im = Image.open(p).convert('RGBA')
    if key in RELEASE_PREP:
        im = _PREP_CACHE[key] if key in _PREP_CACHE else _PREP_CACHE.setdefault(key, RELEASE_PREP[key](im))
    bb = im.getchannel('A').point(lambda v: 255 if v > 8 else 0).getbbox()
    if bb:                              # to the icon's own bounds (an app icon fills the tile)
        im = im.crop(bb)
    s = T / max(im.size)
    w, h = max(1, round(im.size[0] * s)), max(1, round(im.size[1] * s))
    im = im.convert('RGBa').resize((w, h), Image.LANCZOS).convert('RGBA')   # premultiplied
    c = Image.new('RGBA', (T, T), (0, 0, 0, 0))
    c.paste(im, ((T - w) // 2, (T - h) // 2))
    out = np.asarray(c).astype(np.float64)
    out[out[..., 3] == 0] = 0
    return out, mode


def credits():
    """The INFO lines: one per logo (and per release icon present)."""
    return ['logos (tools/brand/logos/SOURCES.md; rasterised, nominative use, no endorsement):'] + \
        ['  ' + CREDITS[n] for n in FILES] + \
        ['  ' + RELEASE[k][3] for k in RELEASE if RELEASE[k][0] and (SRC / RELEASE[k][0]).exists()]


def _kali_dragon(s):
    """kali.org's kali-dragon-icon.svg: only the crisp white dragon (the last path of its last
    <g ... fill="#fff">, without the blurred copy under it), in the icon's coordinates, without
    the blue sphere, its shadows and their filters."""
    vb = re.search(r'<svg\b[^>]*\bviewBox="([^"]+)"', s).group(1)
    gs = list(re.finditer(r'<g transform="([^"]+)" fill="#fff">(.*?)</g>', s, re.S))
    assert len(gs) == 1, 'kali: dragon group'
    paths = re.findall(r'<path\b[^>]*/>', gs[0].group(2))
    assert len(paths) == 2 and 'filter=' in paths[0] and 'filter=' not in paths[1], 'kali: paths'
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{vb}">'
            f'<g transform="{gs[0].group(1)}" fill="#fff">{paths[1]}</g></svg>')


def _drop_groups(*ids):
    """Remove the <g> elements with these ids, with everything inside them."""
    def f(s):
        for gid in ids:
            m = re.search(r'<g\b[^>]*\bid="%s"[^>]*>' % re.escape(gid), s)
            assert m, gid
            depth = 0
            for t in re.finditer(r'<g\b[^>]*>|</g>', s[m.start():]):
                depth += -1 if t.group(0) == '</g>' else 1
                if depth == 0:
                    s = s[:m.start()] + s[m.start() + t.end():]
                    break
            else:
                raise AssertionError(gid)
        return s
    return f


def _drop(text):
    def f(s):
        assert s.count(text) == 1, text
        return s.replace(text, '')
    return f


# Edits (whole elements only) and the part of the render to keep.
#   crop 'top':  what lies above the widest empty band of rows (Debian: the swirl above the
#                wordmark).
PREP = {
    'microsoft': (_drop('<path fill="#f3f3f3" d="M0 0h23v23H0z"/>'), None),   # backdrop square
    'ubuntu': (_drop('<rect class="cls-1" width="1038.90491" height="1600"/>'), None),  # the tag
    'arch': (_drop_groups('g2809', 'g5326', 'text2634', 'text2638'), None),   # the wordmark, (TM)
    'debian': (None, 'top'),
    'kali': (_kali_dragon, None),
}


def source_svg(name):
    rel, url, sha = FILES[name]
    p = SRC / rel
    if not p.exists():
        sys.exit(f'menu7_logos: {p} missing (committed under tools/brand/logos/src/)')
    b = p.read_bytes()
    h = hashlib.sha256(b).hexdigest()
    assert h == sha, f'{p}: sha256 {h}, expected {sha}'
    s = b.decode('utf-8')
    assert not BAD.search(s), f'{p}: external reference or script'
    edit = PREP.get(name, (None, None))[0]
    return edit(s) if edit else s


def _rsvg(svg, args):
    with tempfile.TemporaryDirectory() as td:
        i, o = Path(td) / 'a.svg', Path(td) / 'a.png'
        i.write_text(svg)
        subprocess.run(['rsvg-convert', *args, str(i), '-o', str(o)], check=True)
        im = Image.open(o)
        im.load()
        assert im.mode == 'RGBA', im.mode
        return np.asarray(im).astype(np.float64)


def _above_gap(v):
    """Rows (y0, y1) above the widest run of empty rows inside the drawing."""
    nz = np.nonzero(v > 0)[0]
    d = np.diff(nz)
    k = int(np.argmax(d))
    assert d[k] > 1, 'no gap'
    return nz[0], nz[k] + 1


_BBOX = {}


def bbox(name):
    """The logo's bounds (x, y, w, h) in SVG px (zoom 1), the svg text."""
    if name in _BBOX:
        return _BBOX[name]
    svg = source_svg(name)
    one = _rsvg(svg, [])
    z = 2048 / max(one.shape[:2])
    big = _rsvg(svg, ['-z', repr(float(z))])
    a = big[..., 3]
    crop = PREP.get(name, (None, None))[1]
    cols, rows = a.max(axis=0), a.max(axis=1)
    if crop == 'top':
        y0, y1 = _above_gap(rows)
        nzc = np.nonzero(a[y0:y1].max(axis=0))[0]
        x0, x1 = nzc[0], nzc[-1] + 1
    else:
        nzc, nzr = np.nonzero(cols)[0], np.nonzero(rows)[0]
        x0, x1, y0, y1 = nzc[0], nzc[-1] + 1, nzr[0], nzr[-1] + 1
    r = (x0 / z, y0 / z, (x1 - x0) / z, (y1 - y0) / z, svg, crop)
    _BBOX[name] = r
    return r


def render(name, W, H, box, fit=1.0, dx=0.0, dy=0.0, color=None):
    """The logo on a W x H transparent canvas, fitted (aspect kept) into fit x the square box of
    side `box` centred on the canvas, moved by (dx, dy) box units; straight RGBA float 0..255.
    color: '#rrggbb' to paint it in one colour (the Apple logo in white), alpha kept."""
    bx, by, bw, bh, svg, crop = bbox(name)
    s = fit * box / max(bw, bh)
    left = (W - bw * s) / 2 - bx * s + dx * box
    top = (H - bh * s) / 2 - by * s + dy * box
    out = _rsvg(svg, ['-z', repr(float(s)), '--left=%r' % float(left), '--top=%r' % float(top),
                      '--page-width', str(W), '--page-height', str(H)])
    assert out.shape[:2] == (H, W), out.shape
    if crop:   # keep only the measured part (the wordmark beside or below it is cut away)
        m = np.zeros((H, W), bool)
        x0, y0 = int(np.floor(left + bx * s)) - 1, int(np.floor(top + by * s)) - 1
        x1, y1 = int(np.ceil(left + (bx + bw) * s)) + 1, int(np.ceil(top + (by + bh) * s)) + 1
        m[max(y0, 0):max(y1, 0), max(x0, 0):max(x1, 0)] = True
        out[~m] = 0
    if color:
        c = color.lstrip('#')
        out[..., 0], out[..., 1], out[..., 2] = (int(c[i:i + 2], 16) for i in (0, 2, 4))
    out[out[..., 3] == 0] = 0
    return out


def fetch():
    """Download every source again and check it against FILES (and the committed copy). A
    mismatch exits 1 and keeps nothing: the pinned copy in tools/brand/logos/src/ stays."""
    bad = []
    for name, (rel, url, sha) in FILES.items():
        p = SRC / rel
        with tempfile.TemporaryDirectory() as td:
            t = Path(td) / 'a.svg'
            r = subprocess.run(['curl', '-sSLf', '--max-time', '60', '--max-filesize', '3000000',
                                '-A', 'starbite-logo-fetch/1.0 (one-off asset build)', '-o', str(t),
                                url])
            if r.returncode or not t.exists():
                print(f'{name}: {url} download failed (curl {r.returncode})')
                bad.append(name)
                continue
            b = t.read_bytes()
        h = hashlib.sha256(b).hexdigest()
        if h != sha:
            print(f'{name}: {url} sha256 {h}  MISMATCH (expected {sha}); not kept')
            bad.append(name)
            continue
        if not p.exists() or p.read_bytes() != b:
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(b)
        print(f'{name}: ok')
    if bad:
        sys.exit(f'menu7_logos: {len(bad)} source(s) changed upstream or failed: {", ".join(bad)}')


def sheet(out):
    T = 160
    names = list(FILES)
    im = np.zeros((2 * T, len(names) * T, 4))
    im[:T, ..., :3] = 255
    im[T:, ..., :3] = (30, 34, 48)
    im[..., 3] = 255
    for i, n in enumerate(names):
        r = render(n, T, T, T, .8)
        for row in (0, T):
            d = im[row:row + T, i * T:(i + 1) * T]
            a = r[..., 3:4] / 255
            d[..., :3] = r[..., :3] * a + d[..., :3] * (1 - a)
    Image.fromarray(np.clip(np.rint(im), 0, 255).astype(np.uint8)).save(out)


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--fetch', action='store_true')
    ap.add_argument('--sheet')
    a = ap.parse_args()
    if a.fetch:
        fetch()
    if a.sheet:
        sheet(a.sheet)
