#!/usr/bin/env python3
"""A small fake microSD card for card-test.c: an MBR with p1 (exFAT, type 0x07) and p2 (type 0xDA, the OS
store's partition, filled with a pattern), and in p1 a hand-made exFAT volume (Microsoft's specification 3.1, 4,
6, 7) whose /starbite/linux folder holds files in every shape the resolver must tell apart:

    base.erofs     NoFatChain, one run                     -> offered
    repo.erofs     a FAT chain that is one run             -> offered (verified link by link)
    overlay.img    NoFatChain, one run                     -> offered, the write grant's extent
    frag.img       a FAT chain with a jump                 -> refused (fragmented)
    long.img       a FAT chain that goes on past the size  -> refused
    tiny.img       100 bytes                               -> refused
    sub            a folder                                -> refused

    card-test-image.py OUT.img [--p1-start SECTORS]   (default 2048; 2049 makes every file start off a 4 KiB boundary)
File bytes are pattern(id, offset), so the test can check what a run reads. Prints the layout as C #defines.
"""
import struct, sys

out = sys.argv[1]
p1 = int(sys.argv[sys.argv.index('--p1-start') + 1]) if '--p1-start' in sys.argv else 2048
SPC_SHIFT = 6                       # 32 KiB clusters
CB = 512 << SPC_SHIFT
FAT_OFF, HEAP_OFF, CLUSTERS = 128, 2048, 400
P1_LEN = HEAP_OFF + (CLUSTERS << SPC_SHIFT)
P2 = ((p1 + P1_LEN + 2047) // 2048) * 2048
P2_LEN = 8192
img = bytearray((P2 + P2_LEN) * 512)

def pattern(fid, n):
    return bytes(((fid * 131 + i * 7 + (i >> 9)) & 0xff) for i in range(n))

# MBR
def part(i, typ, start, length):
    img[446 + 16 * i:446 + 16 * i + 16] = struct.pack('<BBBBBBBBII', 0, 0, 0, 0, typ, 0, 0, 0, start, length)
part(0, 0x07, p1, P1_LEN)
part(1, 0xda, P2, P2_LEN)
img[510:512] = b'\x55\xaa'
img[P2 * 512:(P2 + P2_LEN) * 512] = bytes((0xd0 + (i & 15)) for i in range(P2_LEN * 512))

# exFAT boot sector
bs = bytearray(512)
bs[0:3] = b'\xeb\x76\x90'
bs[3:11] = b'EXFAT   '
struct.pack_into('<QQIIIIIIHH', bs, 64, p1, P1_LEN, FAT_OFF, HEAP_OFF - FAT_OFF, HEAP_OFF, CLUSTERS, 2, 0x1234abcd,
                 0x0100, 0)
bs[108], bs[109], bs[110], bs[111] = 9, SPC_SHIFT, 1, 0x80
bs[510:512] = b'\x55\xaa'
img[p1 * 512:p1 * 512 + 512] = bs

fat = [0xfffffff8, 0xffffffff] + [0] * CLUSTERS
def cl_off(c):
    return (p1 + HEAP_OFF + ((c - 2) << SPC_SHIFT)) * 512

def entry_set(name, first, size, is_dir, nofat):
    nm = name.encode('utf-16-le')
    nents = (len(name) + 14) // 15
    e = bytearray(32 * (2 + nents))
    e[0], e[1] = 0x85, 1 + nents
    struct.pack_into('<H', e, 4, 0x10 if is_dir else 0x20)
    e[32], e[33], e[35] = 0xc0, 1 | (2 if nofat else 0), len(name)
    struct.pack_into('<Q', e, 32 + 8, size)
    struct.pack_into('<IQ', e, 32 + 20, first, size)
    for k in range(nents):
        e[64 + 32 * k] = 0xc1
        chunk = nm[30 * k:30 * k + 30]
        e[64 + 32 * k + 2:64 + 32 * k + 2 + len(chunk)] = chunk
    return bytes(e)

nextc = [2]
def alloc(n):
    c = nextc[0]
    nextc[0] += n
    return c

root = alloc(1)
fat[root] = 0xffffffff
d_starbite, d_linux, d_sub = alloc(1), alloc(1), alloc(1)
files = {}
def mkfile(fid, name, size, nofat, chain=None):
    n = (size + CB - 1) // CB
    if chain is None:
        first = alloc(n)
        chain = list(range(first, first + n))
    data = pattern(fid, size)
    for i, c in enumerate(chain[:n]):
        img[cl_off(c):cl_off(c) + len(data[i * CB:(i + 1) * CB])] = data[i * CB:(i + 1) * CB]
    if not nofat:
        for a, b in zip(chain, chain[1:]):
            fat[a] = b
        fat[chain[-1]] = 0xffffffff
    files[name] = (fid, chain[0], size)
    return entry_set(name, chain[0], size, False, nofat)

ents = b''
ents += mkfile(1, 'base.erofs', 5 * CB + 4096 * 3 + 100, True)
ents += mkfile(2, 'repo.erofs', 3 * CB, False)
ents += mkfile(3, 'overlay.img', 64 * CB, True)
a = alloc(2); alloc(1); b = alloc(2)
ents += mkfile(4, 'frag.img', 4 * CB, False, [a, a + 1, b, b + 1])
c = alloc(4)
ents += mkfile(5, 'long.img', 3 * CB, False, [c, c + 1, c + 2, c + 3])
ents += mkfile(6, 'tiny.img', 100, True)
ents += entry_set('sub', d_sub, CB, True, True)
img[cl_off(d_linux):cl_off(d_linux) + len(ents)] = ents
# cp7q: a folder of 200 files (19200 bytes of entries in one 32 KiB cluster): the reader's 16 KiB pieces, with
# an entry set across the piece boundary (f170's at 16320..16415)
d_big = alloc(1)
big = b''.join(entry_set('f%03d.txt' % i, 0, 0, False, True) for i in range(200))
img[cl_off(d_big):cl_off(d_big) + len(big)] = big
e = entry_set('linux', d_linux, CB, True, True) + entry_set('big', d_big, CB, True, True)
img[cl_off(d_starbite):cl_off(d_starbite) + len(e)] = e
# the root: bitmap and up-case entries first (the reader skips them), a label, then the folder
r = bytes([0x81]) + bytes(31) + bytes([0x82]) + bytes(31) + bytes([0x83, 4]) + 'TEST'.encode('utf-16-le') + bytes(22)
r += entry_set('starbite', d_starbite, CB, True, True)
img[cl_off(root):cl_off(root) + len(r)] = r
for i, v in enumerate(fat):
    struct.pack_into('<I', img, (p1 + FAT_OFF) * 512 + 4 * i, v)
open(out, 'wb').write(img)
sec = lambda c: p1 + HEAP_OFF + ((c - 2) << SPC_SHIFT)
print(f'#define T_P1 {p1}ULL\n#define T_P1_LEN {P1_LEN}ULL\n#define T_P2 {P2}ULL\n#define T_P2_LEN {P2_LEN}ULL')
print(f'#define T_HEAP {p1 + HEAP_OFF}ULL\n#define T_FAT {p1 + FAT_OFF}ULL\n#define T_SECTORS {len(img) // 512}ULL')
for name, (fid, first, size) in files.items():
    n = name.split('.')[0].upper()
    print(f'#define T_{n}_ID {fid}\n#define T_{n}_SECTOR {sec(first)}ULL\n#define T_{n}_SIZE {size}ULL')
