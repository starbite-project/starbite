#!/usr/bin/env python3
"""Keep chosen trees the phone wrote on the Ventura card root (caches worth keeping: the icon store, the Metal
shader cache), without taking the whole of what a reset boot left behind (boot 20261105-034221: that state kept
loginwindow from starting).
   ventura-harvest.py PATH...     (the phone in TWRP; chunks: build/test-boots/26t1-dirty.txt + HFS metadata)
1. a clone of the image (cp -c) gets the card's chunks the boot wrote, and is mounted read-only with owners on
2. each PATH tree from it replaces the image's copy (rsync, image mounted read-write)
3. owners/modes of every copied item go into build/macos13/owners-desktop.json (root26-cycle applies them)"""
import json, os, subprocess, sys
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IMG = f'{ROOT}/build/macos13/root-desktop.img'
OWNERS = f'{ROOT}/build/macos13/owners-desktop.json'
START, C = 216442880, 64 << 20
SP = os.environ.get('SCRATCH', '/tmp/scratch/0aae28d5-e6d6-46d1-9a0c-4b969fb34afe/scratchpad')
clone, cv, rw = f'{SP}/harvest.img', f'{SP}/harvest-card', f'{SP}/harvest-img'
paths = sys.argv[1:] or sys.exit(__doc__)
dirty = {int(w) for w in open(f'{ROOT}/build/test-boots/26t1-dirty.txt').read().split()}
assert subprocess.run(['adb', 'get-state'], capture_output=True, text=True).stdout.strip() == 'recovery', 'phone not in TWRP'
subprocess.run(['cp', '-c', IMG, clone], check=True)
with open(clone, 'r+b') as f:
    for k in sorted(dirty | {0, 1, 2, 3, os.path.getsize(IMG) // C - 1}):
        d = subprocess.run(['adb', 'exec-out', f'dd if=/dev/block/mmcblk0 bs=1048576 skip={START // 2048 + k * 64} count=64 2>/dev/null'],
                           capture_output=True).stdout
        assert len(d) == C, (k, len(d))
        f.seek(k * C); f.write(d)
os.makedirs(cv, exist_ok=True); os.makedirs(rw, exist_ok=True)
att = lambda img, mnt, *o: subprocess.run(['hdiutil', 'attach', '-nobrowse', *o, '-mountpoint', mnt, '-imagekey',
                                           'diskimage-class=CRawDiskImage', img], check=True, capture_output=True)
cvo = cv + '-owners'
os.makedirs(cvo, exist_ok=True)
att(clone, cvo, '-readonly', '-owners', 'on')
# read as anyone (owners off) to copy; real owners from the owners-on mount where readable, else the
# nearest readable parent's (a cache tree belongs to one daemon)
st_owner = {}
for p in paths:
    q = p
    while q not in ('', '/'):
        try:
            st = os.lstat(cvo + q); st_owner[p] = (st.st_uid, st.st_gid); break
        except OSError:
            q = os.path.dirname(q)
subprocess.run(['hdiutil', 'detach', cvo], capture_output=True)
att(clone, cv, '-readonly')
att(IMG, rw)
own = json.load(open(OWNERS))
n = 0
try:
    for p in paths:
        src = cv + p
        if not os.path.exists(src):
            print('not on the card:', p); continue
        os.makedirs(os.path.dirname(rw + p), exist_ok=True)
        subprocess.run(['rsync', '-a', '--delete', '--no-owner', '--no-group', src + ('/' if os.path.isdir(src) else ''),
                        rw + p], check=True)
        for dp, dns, fns in os.walk(src):
            for name in [''] + dns + fns:
                full = os.path.join(dp, name) if name else dp
                st = os.lstat(full)
                uid, gid = st_owner.get(p, (0, 0))
                own[full[len(cv):]] = [uid, gid, st.st_mode & 0o7777]
                n += 1
        print('kept', p)
finally:
    subprocess.run(['hdiutil', 'detach', rw], capture_output=True)
    subprocess.run(['hdiutil', 'detach', cv], capture_output=True)
    os.unlink(clone)
json.dump(own, open(OWNERS, 'w'))
print(f'{n} items with their owners -> {OWNERS}')
