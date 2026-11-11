#!/usr/bin/env python3
"""Are the three Linux files on the card each ONE run of sectors? (the rule of starbite/guest/sbos/guest-card.h,
written again here from the exFAT specification, so the phone's resolver and this check do not share code)

    card-check.py --adb [--dev /dev/block/mmcblk0] [--verify DIR]     the card in the phone (TWRP, p1 UNMOUNTED)
    card-check.py --image CARD.img [--verify DIR] [NAME ...]          a card image (the host test's fake card)

Reads only: the MBR, p1's exFAT boot sector, the folders /starbite/linux and the files' FAT entries (over adb:
`dd if=DEV bs=512 skip=N count=M`). Per file: PASS with its first sector, sector count, how it is known to be
one run (NoFatChain / FAT chain verified) and whether it starts on a 4 KiB boundary of the card (the overlay must,
to be writable); or FAIL with the reason. --verify DIR: also SHA-256 of the run's bytes on the card against
DIR/<name> (proves the run IS the file). Exit 0 only when base.erofs and overlay.img pass (repo.erofs may be absent).
"""
import argparse, hashlib, os, struct, subprocess, sys

ap = argparse.ArgumentParser()
ap.add_argument('--adb', action='store_true')
ap.add_argument('--dev', default='/dev/block/mmcblk0')
ap.add_argument('--image')
ap.add_argument('--verify')
ap.add_argument('names', nargs='*')
a = ap.parse_args()
names = a.names or ['base.erofs', 'overlay.img', 'repo.erofs']

if a.image:
    f = open(a.image, 'rb')
    def rd(sector, count):
        f.seek(sector * 512)
        return f.read(count * 512)
    def sha_run(sector, nbytes):
        h = hashlib.sha256()
        f.seek(sector * 512)
        left = nbytes
        while left:
            b = f.read(min(left, 1 << 20))
            if not b:
                break
            h.update(b)
            left -= len(b)
        return h.hexdigest()
elif a.adb:
    def rd(sector, count):
        out = subprocess.run(['adb', 'exec-out', f'dd if={a.dev} bs=512 skip={sector} count={count} 2>/dev/null'],
                             capture_output=True, check=True).stdout
        if len(out) != count * 512:
            sys.exit(f'card-check: read of {count} sectors at {sector} returned {len(out)} bytes (is {a.dev} the card? is adb root?)')
        return out
    def sha_run(sector, nbytes):
        # whole MiB with bs=1M when the run starts on a MiB of the card, else 4 KiB or 512-byte blocks; head cuts the tail
        for bs in (1 << 20, 4096, 512):
            if (sector * 512) % bs == 0:
                break
        cmd = f'dd if={a.dev} bs={bs} skip={sector * 512 // bs} count={(nbytes + bs - 1) // bs} 2>/dev/null | head -c {nbytes} | sha256sum'
        return subprocess.run(['adb', 'shell', cmd], capture_output=True, check=True, text=True).stdout.split()[0]
else:
    sys.exit('card-check: --adb or --image CARD.img')

mbr = rd(0, 1)
if mbr[510:512] != b'\x55\xaa' or mbr[3:8] == b'EXFAT':
    sys.exit('card-check: FAIL the card has no partition table')
p1 = None
parts = []
for i in range(4):
    typ, start, length = mbr[446 + 16 * i + 4], *struct.unpack_from('<II', mbr, 446 + 16 * i + 8)
    if length:
        parts.append((i + 1, typ, start, length))
    if typ == 7 and start and length and p1 is None:
        p1 = (start, length)
for n, typ, start, length in parts:
    print(f'card-check: p{n} type {typ:#04x} sectors {start} + {length}')
if not p1:
    sys.exit('card-check: FAIL no exFAT partition (type 0x07)')
vol, vlen = p1
bs = rd(vol, 1)
if bs[3:8] != b'EXFAT' or bs[510:512] != b'\x55\xaa' or bs[108] != 9:
    sys.exit('card-check: FAIL p1 is not exFAT with 512-byte sectors')
fat_off, _fat_len, heap_off, clusters, root = struct.unpack_from('<IIIII', bs, 80)
spc = 1 << bs[109]
cb = spc * 512
print(f'card-check: exFAT at sector {vol}: cluster {cb // 1024} KiB, {clusters} clusters, heap at sector {vol + heap_off}'
      f' ({"4 KiB aligned" if (vol + heap_off) % 8 == 0 else "NOT 4 KiB aligned: no file can be written by the phone"})')
if cb > 128 * 1024:
    print('card-check: WARNING clusters above 128 KiB: Starbite OS\'s exFAT reader does not mount this volume')

def cl_sector(c):
    return vol + heap_off + (c - 2) * spc
def fat(c):
    return struct.unpack_from('<I', rd(vol + fat_off + c // 128, 1), (c % 128) * 4)[0]
def valid(c):
    return 2 <= c < clusters + 2

def listdir(first, nofat, size):
    """entries of a folder: name -> (is_dir, nofat, first, size, valid_size)"""
    out, c, pos, raw = {}, first, 0, b''
    for _ in range(clusters):
        if not valid(c) or (size and pos >= size):
            break
        raw += rd(cl_sector(c), spc)
        pos += cb
        c = c + 1 if nofat else fat(c)
    i = 0
    while i + 32 <= len(raw):
        t = raw[i]
        if t == 0:
            break
        if t == 0x85:
            n2 = raw[i + 1]
            attr = struct.unpack_from('<H', raw, i + 4)[0]
            st = raw[i + 32:i + 64]
            if st[0] == 0xc0:
                name = b''.join(raw[i + 64 + 32 * k + 2:i + 64 + 32 * k + 32] for k in range(n2 - 1))
                name = name.decode('utf-16-le', 'replace')[:st[3]]
                out[name.lower()] = (bool(attr & 0x10), bool(st[1] & 2), struct.unpack_from('<I', st, 20)[0],
                                     struct.unpack_from('<Q', st, 24)[0], struct.unpack_from('<Q', st, 8)[0])
            i += 32 * (1 + n2)
        else:
            i += 32
    return out

d = listdir(root, False, 0)
for part in ('starbite', 'linux'):
    e = d.get(part)
    if not e or not e[0]:
        sys.exit(f'card-check: FAIL no folder /starbite/linux on p1 (missing: {part})')
    d = listdir(e[2], e[1], e[3])

ok = {}
for name in names:
    e = d.get(name.lower())
    why = None
    if not e:
        why = 'not on the card'
    elif e[0]:
        why = 'it is a folder'
    elif e[3] < 4096:
        why = 'smaller than 4 KiB'
    else:
        _, nofat, first, size, vsize = e
        n = (size + cb - 1) // cb
        if not valid(first) or not valid(first + n - 1):
            why = 'its clusters are outside the volume'
        elif vsize != size:
            why = f'its valid data length ({vsize}) is not its size ({size}): the tail was never written'
        elif not nofat:
            raw = rd(vol + fat_off + first // 128, (first + n - 1) // 128 - first // 128 + 1)
            base = (first // 128) * 128
            for k in range(n):
                nxt = struct.unpack_from('<I', raw, (first + k - base) * 4)[0]
                if (nxt != first + k + 1) if k + 1 < n else (nxt != 0xffffffff):
                    why = (f'FRAGMENTED: cluster {first + k} (number {k} of {n}) is followed by {nxt:#x}' if k + 1 < n
                           else 'its FAT chain does not end at the last cluster')
                    break
        s0, ns = cl_sector(first), size // 4096 * 8
        if not why and not (vol <= s0 and s0 + ns <= vol + vlen):
            why = 'its sectors are outside p1'
        if not why and a.verify:
            p = os.path.join(a.verify, name)
            h = hashlib.sha256()
            with open(p, 'rb') as lf:
                for b in iter(lambda: lf.read(1 << 20), b''):
                    h.update(b)
            if os.path.getsize(p) != size:
                why = f'its size on the card ({size}) is not {p}\'s ({os.path.getsize(p)})'
            elif sha_run(s0, size) != h.hexdigest():
                why = f'the run\'s bytes on the card are not {p} (SHA-256 differs)'
    if not why and name == 'overlay.img' and s0 % 8:
        why = (f'one run at sector {s0}, but NOT on a 4 KiB boundary of the card: the phone can read it, never '
               'write it (the guest would run on a tmpfs upper layer)')
    if why:
        print(f'card-check: FAIL {name}: {why}')
    else:
        ok[name] = True
        print(f'card-check: PASS {name}: one run, {"NoFatChain" if nofat else "FAT chain verified link by link"}, sectors {s0} + {ns}'
              f' ({size} bytes), {"starts on a 4 KiB boundary" if s0 % 8 == 0 else "NOT on a 4 KiB boundary (readable; not writable)"}'
              f'{", SHA-256 of the run = the file" if a.verify else ""}')
need = [n for n in ('base.erofs', 'overlay.img') if n in names]
sys.exit(0 if all(n in ok for n in need) else 1)
