#!/usr/bin/env python3
"""Which 64 MiB chunks of the macOS root differ between the card and the image: the phone hashes
the card (TWRP sha256sum, nothing crosses USB) while the Mac hashes the image, in parallel.
  card-root-compare.py [IMAGE] > chunk numbers that differ (one per line), progress on stderr"""
import hashlib, os, subprocess, sys, threading
IMG = sys.argv[1] if len(sys.argv) > 1 else '/Volumes/Untitled/starbite-macos26/root-26t1.img'
START = 182888448            # sectors; the root's 10 GiB from there
C = 64 << 20
n = os.path.getsize(IMG) // C
img, card = {}, {}
def mac():
    with open(IMG, 'rb') as f:
        for k in range(n):
            img[k] = hashlib.sha256(f.read(C)).hexdigest()
def phone():
    first_mib = START // 2048
    script = f'for k in $(seq 0 {n - 1}); do dd if=/dev/block/mmcblk0 bs=1048576 skip=$(({first_mib} + k * 64)) count=64 2>/dev/null | sha256sum | cut -c1-64; done'
    out = subprocess.run(['adb', 'shell', script], capture_output=True, text=True).stdout.split()
    for k, h in enumerate(out):
        card[k] = h
ts = [threading.Thread(target=mac), threading.Thread(target=phone)]
for t in ts: t.start()
for t in ts: t.join()
diff = [k for k in range(n) if img.get(k) != card.get(k)]
print(f'{len(diff)} of {n} chunks differ', file=sys.stderr)
print('\n'.join(map(str, diff)))
