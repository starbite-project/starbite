#!/usr/bin/env python3
"""The md5 list of sbsim's sbp9 tree (sbsim/sim_p9.c), for the guest's checks (v3test/p9-sbinit): one
"<md5>  <path>" line per file, paths relative to the mount, sorted (md5sum -c reads it). Keep in step with
sim_p9.c's sp9_init: generated content = 8-byte block b of a file with seed s is splitmix64((s << 40) ^ b), LE.
   p9-expect.py > p9.md5"""
import hashlib
import struct

M = (1 << 64) - 1


def mix(z):
    z = (z + 0x9e3779b97f4a7c15) & M
    z = ((z ^ (z >> 30)) * 0xbf58476d1ce4e5b9) & M
    z = ((z ^ (z >> 27)) * 0x94d049bb133111eb) & M
    return z ^ (z >> 31)


def gen(seed, size):
    blocks = (size + 7) // 8
    return b''.join(struct.pack('<Q', mix(((seed << 40) ^ b) & M)) for b in range(blocks))[:size]


files = {'hello.txt': b'Hello from Starbite OS (sbsim)\n',
         'big.bin': gen(1, 3 << 20),
         'Documents/report.bin': gen(2, 100000),
         'Documents/sub/deep.bin': gen(3, 4097),
         'Long/' + 'L' * 251 + '.bin': gen(4, 777)}
for i in range(24):
    files['Pictures/photo-%02d.bin' % i] = gen(100 + i, 1000 * (i + 1) + i)
for p in sorted(files):
    print('%s  ./%s' % (hashlib.md5(files[p]).hexdigest(), p))
