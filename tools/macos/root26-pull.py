#!/usr/bin/env python3
"""Keep what the last boot(s) wrote: the root chunks the phone wrote (build/test-boots/26t1-dirty.txt,
from kernel 0384's bitmap via boot-next.sh) are read back from the card into the image, then the
image's HFS is repaired (every test boot ends in a reset: fsck_hfs -fy replays/fixes) and the list
is cleared, since the card and the image now agree there. For state worth keeping (shader caches,
first-login setup); a normal root26-cycle instead throws those writes away.   root26-pull.py"""
import hashlib, os, subprocess, sys
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IMG = os.environ.get('ROOTIMG', '/Volumes/Untitled/starbite-macos26/root-26t1.img')
DIRTY = f'{ROOT}/build/test-boots/26t1-dirty.txt'
START = 182888448
C = 64 << 20
words = open(DIRTY).read().split() if os.path.exists(DIRTY) else []
if not words: sys.exit('nothing to pull (no dirty chunks recorded)')
if 'ALL' in words: sys.exit('a boot left no bitmap: what it wrote is unknown, refusing to pull')
assert subprocess.run(['adb', 'get-state'], capture_output=True, text=True).stdout.strip() == 'recovery', 'phone not in TWRP'
chunks = sorted({int(w) for w in words})
print('pulling chunks', chunks)
with open(IMG, 'r+b') as f:
    for k in chunks:
        first_mib = START // 2048 + k * 64
        data = subprocess.run(['adb', 'exec-out', f'dd if=/dev/block/mmcblk0 bs=1048576 skip={first_mib} count=64 2>/dev/null'],
                              capture_output=True).stdout
        phone = subprocess.run(['adb', 'shell', f'dd if=/dev/block/mmcblk0 bs=1048576 skip={first_mib} count=64 2>/dev/null | sha256sum'],
                               capture_output=True, text=True).stdout.split()[0]
        if len(data) != C or hashlib.sha256(data).hexdigest() != phone:
            sys.exit(f'chunk {k}: read back {len(data)} bytes, hash mismatch: stopping (image partly updated; rerun)')
        f.seek(k * C); f.write(data)
        print(f'  chunk {k} pulled')
dev = subprocess.run(['hdiutil', 'attach', '-nomount', '-imagekey', 'diskimage-class=CRawDiskImage', IMG],
                     capture_output=True, text=True).stdout.split()[0]
try:
    r = subprocess.run(['fsck_hfs', '-fy', dev.replace('disk', 'rdisk')], capture_output=True, text=True)
    print(r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr)
finally:
    subprocess.run(['hdiutil', 'detach', dev], capture_output=True)
os.unlink(DIRTY)
print('image now holds the boot\'s writes; dirty list cleared')
