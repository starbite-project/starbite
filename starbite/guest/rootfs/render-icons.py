#!/usr/bin/python3
"""Store icons for mk-repo.sh (runs in the trixie dev root: rsvg-convert, PIL).

    render-icons.py LIST EXTRACTED_DIR BASE_DIR OUT_DIR

For every entry of the store list: find its .desktop file (in EXTRACTED_DIR/KEY, the icon/desktop paths of its
packages, or in BASE_DIR for base apps), take Icon=, look the icon up (hicolor scalable SVG first, then the biggest
PNG, then pixmaps; Adwaita as a fallback) and write OUT_DIR/KEY.png (160x160 RGBA) and OUT_DIR/KEY.argb
(160*160 little-endian words 0xAARRGGBB, premultiplied: sbshm.h's icon pool format)."""
import glob
import io
import os
import struct
import subprocess
import sys

from PIL import Image

PX = 160


def desktop_icon(roots, key, main_pkg):
    cands = []
    for r in roots:
        cands += glob.glob(os.path.join(r, "usr/share/applications/*.desktop"))
    # prefer the file named after the package, then any whose name contains it
    def rank(p):
        b = os.path.basename(p)[:-8].lower()
        if b == main_pkg or b == key:
            return 0
        if main_pkg.split("-")[-1] in b:
            return 1
        return 2
    best = None
    for p in sorted(cands, key=rank):
        try:
            txt = open(p, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        if "NoDisplay=true" in txt and rank(p) > 0:
            continue
        sect = txt.split("[Desktop Entry]", 1)[-1].split("\n[", 1)[0]
        for line in sect.splitlines():
            if line.startswith("Icon="):
                if rank(p) < 2 or best is None:
                    return line[5:].strip(), os.path.basename(p)
    return None, None


def find_icon(roots, name):
    if os.path.isabs(name):
        for r in roots:
            p = os.path.join(r, name.lstrip("/"))
            if os.path.exists(p):
                return p
    for theme in ("hicolor", "Adwaita"):
        for r in roots:
            for ctx in ("apps", "places", "mimetypes", "devices", "categories"):
                p = os.path.join(r, f"usr/share/icons/{theme}/scalable/{ctx}/{name}.svg")
                if os.path.exists(p):
                    return p
        best, bsz = None, 0
        for r in roots:
            for p in glob.glob(os.path.join(r, f"usr/share/icons/{theme}/*/apps/{name}.png")):
                d = os.path.basename(os.path.dirname(os.path.dirname(p)))
                try:
                    sz = int(d.split("x")[0].split("@")[0])
                except ValueError:
                    sz = 1
                if sz > bsz:
                    best, bsz = p, sz
        if best:
            return best
    for r in roots:
        for ext in ("svg", "png", "xpm"):
            p = os.path.join(r, f"usr/share/pixmaps/{name}.{ext}")
            if os.path.exists(p):
                return p
        g = glob.glob(os.path.join(r, f"usr/share/*/icons/**/{name}.svg"), recursive=True)
        if g:
            return g[0]
    return None


def render(path):
    if path.endswith(".svg") or path.endswith(".svgz"):
        png = subprocess.run(["rsvg-convert", "-a", "-w", str(PX), "-h", str(PX), path],
                             check=True, capture_output=True).stdout
        im = Image.open(io.BytesIO(png)).convert("RGBA")
    else:
        im = Image.open(path).convert("RGBA")
        im.thumbnail((PX, PX), Image.LANCZOS) if max(im.size) > PX else None
        if max(im.size) < PX:
            s = PX / max(im.size)
            im = im.resize((round(im.size[0] * s), round(im.size[1] * s)), Image.LANCZOS)
    out = Image.new("RGBA", (PX, PX), (0, 0, 0, 0))
    out.paste(im, ((PX - im.size[0]) // 2, (PX - im.size[1]) // 2), im)
    return out


def argb(im):
    b = bytearray()
    for r, g, bl, a in im.getdata():
        # premultiplied, little-endian 0xAARRGGBB = bytes B, G, R, A
        b += struct.pack("<I", (a << 24) | ((r * a + 127) // 255 << 16) | ((g * a + 127) // 255 << 8) |
                         ((bl * a + 127) // 255))
    return bytes(b)


def main():
    lst, xdir, base, outd = sys.argv[1:5]
    os.makedirs(outd, exist_ok=True)
    fails = 0
    for line in open(lst, encoding="utf-8"):
        if not line.strip() or line.startswith("#"):
            continue
        f = line.rstrip("\n").split("\t")
        key, apt = f[0], f[5].split()
        roots = [os.path.join(xdir, key), base]
        icon, desk = desktop_icon(roots, key, apt[0])
        if len(f) > 7 and f[7].strip():     # the list's icon override (a theme name)
            icon = f[7].strip()
        path = find_icon(roots, icon) if icon else None
        if not path:
            print(f"render-icons: {key}: no icon (desktop {desk}, Icon={icon})")
            fails += 1
            continue
        im = render(path)
        im.save(os.path.join(outd, key + ".png"))
        open(os.path.join(outd, key + ".argb"), "wb").write(argb(im))
        print(f"render-icons: {key}: {desk} Icon={icon} <- {path.split('/usr/share/', 1)[-1]}")
    sys.exit(1 if fails else 0)


main()
