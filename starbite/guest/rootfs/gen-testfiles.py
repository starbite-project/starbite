#!/usr/bin/python3
"""Test documents for measure-ram.sh (runs in the dev root: PIL). gen-testfiles.py OUTDIR
page.html (a long article with tables, inline SVG and a little script), notes.md, photo.png (4000x3000, a phone
photo's size), photo-small.png (1600x1200), drawing.svg (2000 shapes), sheet.csv (20000 rows x 8)."""
import math
import os
import random
import sys

from PIL import Image, ImageDraw, ImageFilter

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
random.seed(7)
words = ("starbite phone linux guest kernel overlay store package memory window surface compositor touch keyboard "
         "battery screen core cluster render frame pixel buffer table index icon apt debian install remove queue "
         "progress shared region host guest seqlock").split()


def para(n):
    return " ".join(random.choice(words) for _ in range(n)).capitalize() + "."


# ---- page.html: about the size of a long Wikipedia article ----
h = ["<!doctype html><html><head><meta charset='utf-8'><title>Starbite test article</title><style>",
     "body{font-family:sans-serif;max-width:46em;margin:auto;line-height:1.5} table{border-collapse:collapse}",
     "td,th{border:1px solid #aaa;padding:2px 6px} .box{float:right;width:220px;margin:0 0 1em 1em;"
     "background:#f4f4f4;border:1px solid #ccc;padding:8px} h2{border-bottom:1px solid #ccc}",
     "</style></head><body><h1>Starbite test article</h1>"]
for s in range(30):
    h.append(f"<h2>Section {s + 1}</h2>")
    if s % 3 == 0:
        h.append("<div class='box'><svg width='200' height='140' viewBox='0 0 200 140'>")
        for k in range(40):
            h.append(f"<circle cx='{random.randint(0, 200)}' cy='{random.randint(0, 140)}' r='{random.randint(3, 20)}' "
                     f"fill='hsl({random.randint(0, 359)},70%,60%)' opacity='0.7'/>")
        h.append("</svg><p>" + para(20) + "</p></div>")
    for p in range(6):
        h.append("<p>" + para(random.randint(60, 140)) + "</p>")
    if s % 4 == 1:
        h.append("<table><tr>" + "".join(f"<th>Col {c}</th>" for c in range(6)) + "</tr>")
        for r in range(25):
            h.append("<tr>" + "".join(f"<td>{random.randint(0, 99999)}</td>" for c in range(6)) + "</tr>")
        h.append("</table>")
h.append("<canvas id='c' width='600' height='200'></canvas><script>let c=document.getElementById('c').getContext('2d');"
         "let t=0;function f(){c.clearRect(0,0,600,200);for(let i=0;i<60;i++){c.fillStyle=`hsl(${(i*6+t)%360},70%,50%)`;"
         "c.fillRect(i*10,100+80*Math.sin((i+t)/9),8,8)}t++;if(t<600)requestAnimationFrame(f)}f();</script>")
h.append("</body></html>")
open(os.path.join(out, "page.html"), "w").write("\n".join(h))

# ---- notes.md ----
md = ["# Notes", ""]
for s in range(12):
    md += [f"## Topic {s + 1}", "", para(80), "", "- " + para(8), "- " + para(10), "- " + para(6), "",
           "```", "apt-get install " + random.choice(words), "```", ""]
open(os.path.join(out, "notes.md"), "w").write("\n".join(md))

# ---- photo.png: smooth colour fields + noise (does not compress to nothing) ----
W, H = 4000, 3000
small = Image.new("RGB", (W // 20, H // 20))
px = small.load()
for y in range(H // 20):
    for x in range(W // 20):
        px[x, y] = (int(128 + 120 * math.sin(x / 17)), int(128 + 120 * math.sin(y / 23)), int(128 + 100 * math.cos((x + y) / 31)))
big = small.resize((W, H), Image.BICUBIC)
noise = Image.effect_noise((W, H), 24).convert("RGB")
img = Image.blend(big, noise, 0.15)
d = ImageDraw.Draw(img)
for _ in range(300):
    x, y = random.randint(0, W), random.randint(0, H)
    d.ellipse([x, y, x + random.randint(20, 300), y + random.randint(20, 300)],
              outline=(random.randint(0, 255),) * 3, width=4)
img.save(os.path.join(out, "photo.png"), compress_level=1)
img.resize((1600, 1200), Image.LANCZOS).save(os.path.join(out, "photo-small.png"))

# ---- drawing.svg ----
s = ["<svg xmlns='http://www.w3.org/2000/svg' width='1200' height='1600' viewBox='0 0 1200 1600'>"]
for k in range(2000):
    t = random.random()
    if t < 0.4:
        s.append(f"<rect x='{random.randint(0, 1150)}' y='{random.randint(0, 1550)}' width='{random.randint(5, 80)}' "
                 f"height='{random.randint(5, 80)}' fill='hsl({random.randint(0, 359)},60%,55%)' opacity='0.6'/>")
    elif t < 0.8:
        s.append(f"<circle cx='{random.randint(0, 1200)}' cy='{random.randint(0, 1600)}' r='{random.randint(3, 50)}' "
                 f"fill='none' stroke='hsl({random.randint(0, 359)},60%,40%)' stroke-width='3'/>")
    else:
        pts = " ".join(f"{random.randint(0, 1200)},{random.randint(0, 1600)}" for _ in range(6))
        s.append(f"<polyline points='{pts}' fill='none' stroke='black' stroke-width='1.5'/>")
s.append("</svg>")
open(os.path.join(out, "drawing.svg"), "w").write("\n".join(s))

# ---- sheet.csv ----
with open(os.path.join(out, "sheet.csv"), "w") as f:
    f.write("id,name,qty,price,total,date,region,note\n")
    for r in range(20000):
        q, p = random.randint(1, 50), round(random.uniform(1, 500), 2)
        f.write(f"{r},{random.choice(words)},{q},{p},{round(q * p, 2)},2026-{random.randint(1, 12):02d}-"
                f"{random.randint(1, 28):02d},{random.choice(['north', 'south', 'east', 'west'])},{random.choice(words)}\n")
# ---- slides.fodp: a flat-XML presentation, 10 slides with a title and bullet text ----
fo = ['<?xml version="1.0" encoding="UTF-8"?>',
      '<office:document xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" '
      'xmlns:draw="urn:oasis:names:tc:opendocument:xmlns:drawing:1.0" '
      'xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0" '
      'xmlns:svg="urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0" '
      'xmlns:presentation="urn:oasis:names:tc:opendocument:xmlns:presentation:1.0" '
      'xmlns:style="urn:oasis:names:tc:opendocument:xmlns:style:1.0" '
      'xmlns:fo="urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0" '
      'office:version="1.3" office:mimetype="application/vnd.oasis.opendocument.presentation">',
      '<office:body><office:presentation>']
for i in range(10):
    fo.append(f'<draw:page draw:name="Slide {i + 1}">')
    fo.append('<draw:frame svg:x="1cm" svg:y="1cm" svg:width="26cm" svg:height="3cm"><draw:text-box>'
              f'<text:p>Slide {i + 1}: {para(4)}</text:p></draw:text-box></draw:frame>')
    fo.append('<draw:frame svg:x="1cm" svg:y="5cm" svg:width="26cm" svg:height="12cm"><draw:text-box>' +
              "".join(f"<text:p>- {para(9)}</text:p>" for _ in range(5)) + '</draw:text-box></draw:frame>')
    fo.append(f'<draw:ellipse svg:x="{2 + i}cm" svg:y="16cm" svg:width="3cm" svg:height="2cm"/></draw:page>')
fo.append('</office:presentation></office:body></office:document>')
open(os.path.join(out, "slides.fodp"), "w").write("\n".join(fo))
print("gen-testfiles:", ", ".join(sorted(os.listdir(out))))
