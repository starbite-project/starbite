#!/usr/bin/env python3
# sbtk-fakecard.py OUT: a made-up microSD card for sbtk-host's Photos, Files and Music previews (host/main.c reads
# OUT as the card, read only; SBTK_CARD=DIR another folder, SBTK_CARD=none no card). Everything is generated here:
# the pictures are drawn (skies, hills, a sun), the songs are silent, the names and dates are invented.
#   DCIM/Camera      camera-like JPEGs: 12 MP with an EXIF thumbnail (two held upright: orientation 6), two plain
#                    ones without EXIF, a grey one, a progressive one (which Photos must say it cannot show)
#   DCIM/Trip        26 smaller JPEGs (the grid scrolls)
#   Pictures         a full-screen PNG "screenshot", two small pictures
#   Music            MP3s with ID3v2.3 / v2.4 tags, an untagged one, a WAV, a FLAC and an M4A header
#   Documents, Download, the root: a few other files for Files' icons
import io, os, struct, sys, time
import numpy as np
from PIL import Image, ImageDraw

out = sys.argv[1]
rng = np.random.default_rng(2026)

def scene(w, h, seed, hue):
    r = np.random.default_rng(seed)
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    t = y / h
    top = np.array(hue[0], np.float32); mid = np.array(hue[1], np.float32); low = np.array(hue[2], np.float32)
    img = top[None, None, :] * (1 - t[..., None]) + mid[None, None, :] * t[..., None]
    sx, sy, sr = w * r.uniform(.2, .8), h * r.uniform(.18, .4), min(w, h) * r.uniform(.05, .09)
    d = np.sqrt((x - sx) ** 2 + (y - sy) ** 2)
    glow = np.clip(1 - d / (sr * 5), 0, 1) ** 2
    img += glow[..., None] * np.array([70, 55, 30], np.float32)
    img[d < sr] = np.array([255, 244, 214], np.float32)
    for k in range(3):
        base = h * (.55 + .13 * k)
        ridge = base + h * .06 * np.sin(x[0] / w * r.uniform(3, 9) + r.uniform(0, 6)) + h * .03 * np.sin(x[0] / w * r.uniform(11, 23) + r.uniform(0, 6))
        m = y > ridge[None, :]
        img[m] = low * (1 - .28 * k) + (y[m] - base)[:, None] / h * 40
    img += r.normal(0, 2.2, img.shape[:2])[..., None]
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))

def exif(orient, thumb):
    ifd0 = struct.pack('<H', 1) + struct.pack('<HHII', 0x0112, 3, 1, orient) + struct.pack('<I', 8 + 2 + 12 + 4)
    off = 8 + len(ifd0) + 2 + 24 + 4
    ifd1 = struct.pack('<H', 2) + struct.pack('<HHII', 0x0201, 4, 1, off) + struct.pack('<HHII', 0x0202, 4, 1, len(thumb)) + struct.pack('<I', 0)
    return b'Exif\0\0' + b'II*\0' + struct.pack('<I', 8) + ifd0 + ifd1 + thumb

def put(path, data, when):
    p = os.path.join(out, path)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, 'wb').write(data)
    t = time.mktime(time.strptime(when, '%Y-%m-%d %H:%M'))
    os.utime(p, (t, t))

def jpeg(im, **kw):
    b = io.BytesIO(); im.save(b, 'JPEG', **kw); return b.getvalue()

HUES = [((38, 74, 140), (236, 164, 108), (40, 62, 54)), ((18, 26, 64), (140, 86, 150), (26, 30, 52)),
        ((96, 160, 222), (214, 232, 240), (64, 110, 70)), ((240, 150, 90), (250, 210, 150), (90, 56, 48)),
        ((20, 60, 80), (120, 190, 180), (16, 50, 60)), ((60, 60, 70), (190, 190, 200), (40, 44, 50))]
# the camera's
cam = [('20261018_071233.jpg', '2026-10-18 07:12', 1, 0), ('20261018_071401.jpg', '2026-10-18 07:14', 6, 1),
       ('20261024_163050.jpg', '2026-10-24 16:30', 1, 2), ('20261024_163412.jpg', '2026-10-24 16:34', 1, 3),
       ('20261102_184455.jpg', '2026-11-02 18:44', 6, 4), ('20261109_120007.jpg', '2026-11-09 12:00', 1, 5)]
for i, (name, when, orient, hue) in enumerate(cam):
    # held upright (orientation 6): the sensor's picture lies on its side, the viewer must turn it clockwise
    im = scene(3024, 4032, 100 + i, HUES[hue]).transpose(Image.ROTATE_90) if orient == 6 else scene(4032, 3024, 100 + i, HUES[hue])
    th = jpeg(im.resize((512, 384), Image.LANCZOS), quality=82)
    put('DCIM/Camera/' + name, jpeg(im, quality=88, exif=exif(orient, th)), when)
put('DCIM/Camera/20261005_090000.jpg', jpeg(scene(2048, 1536, 201, HUES[2]), quality=85), '2026-10-05 09:00')
put('DCIM/Camera/20261005_090130.jpg', jpeg(scene(1536, 2048, 202, HUES[0]), quality=85, subsampling=0), '2026-10-05 09:01')
put('DCIM/Camera/20260930_201500_mono.jpg', jpeg(scene(1600, 1200, 203, HUES[5]).convert('L'), quality=85), '2026-09-30 20:15')
put('DCIM/Camera/20260928_101010_web.jpg', jpeg(scene(1280, 960, 204, HUES[3]), quality=80, progressive=True), '2026-09-28 10:10')
# a trip's worth of smaller ones, so the grid scrolls
for i in range(26):
    w, h = ((1600, 1200), (1200, 1600), (1920, 1080))[i % 3]
    put('DCIM/Trip/IMG_%04d.jpg' % (212 + i), jpeg(scene(w, h, 400 + i, HUES[(i * 5 + 1) % 6]), quality=84),
        '2026-08-%02d %02d:%02d' % (3 + i // 4, 9 + i % 9, (i * 7) % 60))
# pictures
shot = Image.new('RGB', (1440, 2960), (10, 15, 31)); dr = ImageDraw.Draw(shot)
for r in range(9):
    dr.rounded_rectangle([60, 240 + 290 * r, 1380, 480 + 290 * r], 56, fill=(21, 28, 51))
    dr.rounded_rectangle([110, 300 + 290 * r, 230, 420 + 290 * r], 30, fill=tuple(int(v) for v in HUES[r % 6][1]))
    dr.rounded_rectangle([290, 330 + 290 * r, 900 - 50 * (r % 3), 360 + 290 * r], 14, fill=(200, 206, 220))
    dr.rounded_rectangle([290, 386 + 290 * r, 700, 406 + 290 * r], 10, fill=(96, 104, 128))
b = io.BytesIO(); shot.save(b, 'PNG'); put('Pictures/Screenshots/Screenshot_20261103-081500.png', b.getvalue(), '2026-11-03 08:15')
put('Pictures/Wallpapers/Dunes.jpg', jpeg(scene(1440, 2960, 301, HUES[3]), quality=86), '2026-08-14 10:00')
b = io.BytesIO(); scene(640, 480, 302, HUES[4]).save(b, 'PNG'); put('Pictures/Wallpapers/Lagoon.png', b.getvalue(), '2026-08-14 10:02')

# songs: an ID3v2 tag, then a few silent MPEG-1 Layer III frames (128 kbit/s, 44.1 kHz: 417 bytes each)
def frame(fid, text, v, enc):
    body = bytes([enc]) + (text.encode('utf-16') if enc == 1 else text.encode('utf-8' if enc == 3 else 'latin-1'))
    n = len(body)
    size = struct.pack('>I', n) if v == 3 else bytes([n >> 21 & 127, n >> 14 & 127, n >> 7 & 127, n & 127])
    return fid.encode() + size + b'\0\0' + body
def mp3(title, artist, v=3, enc=0, frames=40):
    silent = (b'\xff\xfb\x90\x00' + b'\0' * 413) * frames
    if not title: return silent
    tag = frame('TIT2', title, v, enc) + frame('TPE1', artist, v, enc) + frame('TALB', 'Made-up Album', v, enc)
    n = len(tag)
    return b'ID3' + bytes([v, 0, 0, n >> 21 & 127, n >> 14 & 127, n >> 7 & 127, n & 127]) + tag + silent
put('Music/01 Night Drive.mp3', mp3('Night Drive', 'The Paper Lanterns', 3, 0, 9000), '2026-07-02 21:00')
put('Music/02 Harbour Lights.mp3', mp3('Harbour Lights', 'The Paper Lanterns', 3, 1, 7600), '2026-07-02 21:00')
put('Music/03 Café at Noon.mp3', mp3('Café at Noon', 'Mira Solís', 4, 3, 8200), '2026-07-09 12:30')
put('Music/04 Slow Tide.mp3', mp3('Slow Tide', 'Mira Solís', 4, 3, 10100), '2026-07-09 12:31')
put('Music/demo take 3.mp3', mp3('', ''), '2026-09-12 17:45')
wav = b'RIFF' + struct.pack('<I', 36 + 44100 * 2) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 1, 1, 44100, 88200, 2, 16) + b'data' + struct.pack('<I', 44100 * 2) + b'\0' * (44100 * 2)
put('Music/Field Notes/rain on the roof.wav', wav, '2026-10-01 06:20')
put('Music/Field Notes/Morning Birds.flac', b'fLaC' + bytes(60000), '2026-10-03 05:55')
put('Music/Lullaby for a Small Robot.m4a', b'\0\0\0\x1cftypM4A \0\0\0\0M4A mp42isom' + bytes(240000), '2026-06-21 20:00')
# the rest
put('Documents/Trip plan.txt', b'Day 1: the harbour.\nDay 2: the dunes.\n', '2026-10-12 19:30')
put('Documents/Manual.pdf', b'%PDF-1.4\n% a made-up file for the preview\n' + bytes(180000), '2026-05-03 11:00')
put('Documents/Budget 2026.xlsx', bytes(42000), '2026-01-15 09:00')
put('Download/backup-old-phone.zip', bytes(3 * 1024 * 1024), '2026-04-22 22:10')
put('Download/clip.mp4', bytes(900000), '2026-10-30 18:05')
for i in range(34):
    put('Documents/Receipts/receipt-%03d.pdf' % (i + 1), b'%PDF-1.4\n' + bytes(20000 + 977 * i), '2026-%02d-%02d 10:00' % (1 + i % 10, 1 + (i * 3) % 27))
put('readme.txt', b'This card is made up for the Starbite OS previews.\n', '2026-11-01 08:00')
os.makedirs(os.path.join(out, 'Empty folder'), exist_ok=True)
for d, when in (('DCIM', '2026-11-09 12:00'), ('DCIM/Camera', '2026-11-09 12:00'), ('Pictures', '2026-11-03 08:15'), ('Pictures/Screenshots', '2026-11-03 08:15'),
                ('Pictures/Wallpapers', '2026-08-14 10:02'), ('Music', '2026-10-03 05:55'), ('Music/Field Notes', '2026-10-03 05:55'),
                ('Documents', '2026-10-12 19:30'), ('Download', '2026-10-30 18:05'), ('Empty folder', '2026-03-01 12:00')):
    t = time.mktime(time.strptime(when, '%Y-%m-%d %H:%M')); os.utime(os.path.join(out, d), (t, t))
print('fake card:', out)
