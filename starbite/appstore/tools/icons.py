#!/usr/bin/python3
"""App Store icons (runs inside tools/vm-chroot.sh's trixie root: apt-get download, rsvg-convert, PIL).

    icons.py CATALOG_DIR OUT_DIR [ID...]

For every recipe: download its Debian package (distro.apt[0], then its *-data / *-common dependencies if the icon
is not in it), find the recipe's `icon` (hicolor scalable SVG first, then the biggest PNG, then pixmaps, then the
.desktop file's Icon=), and write OUT_DIR/<id>.png (256x256) and OUT_DIR/<id>.argb (160x160 little-endian words
0xAARRGGBB, premultiplied: the icon pool's format, sbshm.h). Apps without a Debian package (or an icon) get
`icon_url` from the recipe when set, else a lettered tile in the category's colour."""
import glob
import io
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import tomllib
import urllib.request

from PIL import Image, ImageDraw, ImageFont

PX = 160
COL = {"Internet": (0x0a, 0x84, 0xff), "Office": (0xff, 0x9f, 0x0a), "Graphics": (0xbf, 0x5a, 0xf2),
       "Media": (0xff, 0x37, 0x5f), "Games": (0x30, 0xd1, 0x58), "Utilities": (0x8e, 0x8e, 0x93),
       "Development": (0x5e, 0x5c, 0xe6), "Education": (0xff, 0xd6, 0x0a), "Science": (0x64, 0xd2, 0xff),
       "System": (0x63, 0x63, 0x66)}


def sh(*a, cwd=None):
    return subprocess.run(a, cwd=cwd, capture_output=True, text=True)


CACHE = "/var/cache/sbstore-icons"


def extract(pkg, d):
    os.makedirs(CACHE, exist_ok=True)
    have = glob.glob("%s/%s_*.deb" % (CACHE, pkg))
    if not have:
        if sh("apt-get", "download", "-q", pkg, cwd=CACHE).returncode:
            return None
        have = glob.glob("%s/%s_*.deb" % (CACHE, pkg))
    x = os.path.join(d, "x")
    os.makedirs(x, exist_ok=True)
    for deb in have:
        sh("dpkg-deb", "-x", deb, x)
    return x


def find_icon(root, name):
    best = None
    for p in glob.glob(root + "/usr/share/icons/**/*", recursive=True) + glob.glob(root + "/usr/share/pixmaps/*") + \
            glob.glob(root + "/usr/share/*/icons/**/*", recursive=True) + glob.glob(root + "/opt/**/*.png", recursive=True):
        b, ext = os.path.splitext(os.path.basename(p))
        if b != name or ext not in (".svg", ".png", ".svgz") or not os.path.isfile(p):
            continue
        if "symbolic" in p:
            continue
        m = re.search(r"/(\d+)x\d+/", p)
        score = 100000 if ext in (".svg", ".svgz") and "scalable" in p else 50000 if ext.startswith(".svg") else \
            (int(m.group(1)) if m else 48)
        if best is None or score > best[0]:
            best = (score, p)
    return best[1] if best else None


def desktop_icon(root):
    for p in glob.glob(root + "/usr/share/applications/*.desktop"):
        m = re.search(r"^Icon=(.+)$", open(p, errors="replace").read(), re.M)
        if m:
            return m.group(1).strip()
    return None


def render(path, size):
    if path.endswith((".svg", ".svgz")):
        r = subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), "-a", path], capture_output=True)
        im = Image.open(io.BytesIO(r.stdout)).convert("RGBA")
    else:
        im = Image.open(path).convert("RGBA")
    # fit into a square, centred
    im.thumbnail((size, size), Image.LANCZOS)
    c = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    c.paste(im, ((size - im.width) // 2, (size - im.height) // 2), im)
    if im.width < size // 2:                              # tiny pixmap: scale up
        c = im.resize((size, size), Image.LANCZOS)
    return c


def tile(name, cat, size, term=False):
    """no icon upstream: a rounded tile in the category's colour with the initial (terminal apps: dark, ">_")"""
    c = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(c)
    r, g, b = (0x1c, 0x1c, 0x1e) if term else COL.get(cat, (0x8e, 0x8e, 0x93))
    d.rounded_rectangle((0, 0, size - 1, size - 1), radius=size * 0.225, fill=(r, g, b, 255))
    face = "DejaVuSansMono-Bold.ttf" if term else "DejaVuSans-Bold.ttf"
    try:
        f = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/" + face, int(size * (0.3 if term else 0.5)))
    except OSError:
        f = ImageFont.load_default()
    if term:
        d.text((size * 0.16, size * 0.14), ">_", font=f, fill=(0x30, 0xd1, 0x58, 255))
        f2 = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", int(size * 0.17)) \
            if os.path.exists("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf") else f
        bb = d.textbbox((0, 0), name, font=f2)
        d.text(((size - (bb[2] - bb[0])) / 2, size * 0.66), name, font=f2, fill=(255, 255, 255, 255))
        return c
    t = name[:1].upper()
    bb = d.textbbox((0, 0), t, font=f)
    d.text(((size - bb[2] - bb[0]) / 2, (size - bb[3] - bb[1]) / 2 - bb[1] * 0.0), t, font=f, fill=(255, 255, 255, 255))
    return c


def argb(im):
    out = bytearray()
    for r, g, b, a in im.getdata():
        out += struct.pack("<I", (a << 24) | ((r * a // 255) << 16) | ((g * a // 255) << 8) | (b * a // 255))
    return bytes(out)


def main(argv):
    catd, out = argv[0], argv[1]
    os.makedirs(out, exist_ok=True)
    ids = argv[2:]
    for fn in sorted(os.listdir(catd)):
        if not fn.endswith(".toml"):
            continue
        a = tomllib.load(open(os.path.join(catd, fn), "rb"))
        if ids and a["id"] not in ids:
            continue
        d = tempfile.mkdtemp(prefix="icon.")
        src, how = None, "tile"
        try:
            pk = (a.get("distro") or {}).get("apt", [None])[0]
            if a.get("icon_url"):
                p = os.path.join(d, "icon" + os.path.splitext(a["icon_url"])[1])
                urllib.request.urlretrieve(a["icon_url"], p)
                src, how = p, "icon_url"
            elif pk:
                x = extract(pk, d)
                name = a.get("icon") or (x and desktop_icon(x)) or pk
                src = x and (find_icon(x, name) or find_icon(x, desktop_icon(x) or name))
                if x and not src:                         # the icon lives in a -data / -common package
                    deps = sh("apt-cache", "depends", pk).stdout
                    for dep in re.findall(r"Depends: (\S+)", deps):
                        if re.search(r"-(data|common|icons|theme)$", dep):
                            extract(dep, d)
                    src = find_icon(x, name) or find_icon(x, desktop_icon(x) or name)
                if src:
                    how = os.path.relpath(src, x)
            term = bool((a.get("launcher") or {}).get("terminal"))
            big = render(src, 256) if src else tile(a["name"], a.get("category"), 256, term)
            small = render(src, PX) if src else tile(a["name"], a.get("category"), PX, term)
        except Exception as e:                            # noqa: BLE001 (a broken icon must not stop the rest)
            big, small, how = tile(a["name"], a.get("category"), 256), tile(a["name"], a.get("category"), PX), "tile (%s)" % e
        big.save(os.path.join(out, a["id"] + ".png"))
        with open(os.path.join(out, a["id"] + ".argb"), "wb") as f:
            f.write(argb(small))
        print("%-20s %s" % (a["id"], how), flush=True)
        shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    main(sys.argv[1:])
